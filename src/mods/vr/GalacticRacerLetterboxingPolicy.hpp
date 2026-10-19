#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

#include "GalacticRacerLetterboxingCode.hpp"

namespace uevr::swgr_letterbox {
enum class Status { off, inactive_mode, waiting_names, unsupported, hook_failed, armed, active };

inline constexpr uintptr_t known_getter_rva = 0x3b5964b;
inline constexpr char getter_signature[] =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 68 01 00 00 48 89 CE 48 8B 05 ? ? ? ? "
    "48 31 E0 48 89 84 24 60 01 00 00 48 8B 42 08 F6 40 48 01";

struct Chain {
    std::array<uintptr_t, 10> elements{};
    uintptr_t heap{};
    int32_t count{};
    int32_t capacity{};
};
static_assert(sizeof(Chain) == 0x60 && alignof(Chain) == 8);
static_assert(offsetof(Chain, heap) == 0x50 && offsetof(Chain, count) == 0x58 &&
    offsetof(Chain, capacity) == 0x5c);

struct Names {
    int32_t dynamic_class{}, material_class{}, parent{};
    bool valid() const {
        return dynamic_class > 0 && material_class > 0 && parent > 0 &&
            dynamic_class != material_class && dynamic_class != parent && material_class != parent;
    }
};

struct Memory {
    void* context{};
    bool (*read)(void*, uintptr_t, void*, size_t){};
    template<class T> bool load(uintptr_t p, T& value) const {
        return read && p >= 0x10000 && p <= (std::numeric_limits<uintptr_t>::max)() - sizeof(T) &&
            read(context, p, &value, sizeof(T));
    }
};

inline bool pointer(uintptr_t p) { return p >= 0x10000 && p < 0x0000800000000000 && !(p & 7); }
inline bool in_module(uintptr_t p, size_t n, uintptr_t base, size_t size) {
    return base && p >= base && p-base <= size && n <= size-(p-base);
}
inline bool stack_return_buffer(uintptr_t p, uintptr_t low, uintptr_t high) {
    return low && low < high && !(p & 7) && p >= low && p < high && sizeof(Chain) <= high-p;
}
inline bool supported_mode(bool game, bool dx12, bool native, bool hmd, bool mono, bool screen, bool split) {
    return game && dx12 && native && hmd && !mono && !screen && !split;
}
inline bool code_matches(std::span<const uint8_t> bytes) {
    if (bytes.size() != getter_code.size()) { return false; }
    for (size_t i = 0; i < getter_code.size(); ++i) {
        if ((bytes[i] & getter_mask[i]) != (getter_code[i] & getter_mask[i])) { return false; }
    }
    return true;
}

struct StereoIdentity { int32_t pass{}, eye{}, primary{}; };
inline std::optional<unsigned> eye_of_view(const Memory& m, uintptr_t view) {
    uintptr_t family{}, views{};
    int32_t count{};
    StereoIdentity stereo{};
    std::array<int32_t, 4> rect{};
    if (!pointer(view) || !m.load(view+8, family) || !pointer(family) ||
        !m.load(family+8, views) || !pointer(views) || !m.load(family+0x10, count) ||
        !m.load(view+0xdf0, stereo) || !m.load(view+0x1f10, rect) ||
        stereo.eye < 0 || stereo.eye > 1 || stereo.primary != 0 ||
        rect[0] < 0 || rect[1] < 0 || rect[2] <= rect[0] || rect[3] <= rect[1] ||
        rect[2] > 32768 || rect[3] > 32768) { return {}; }
    // Native Fix uses singleton physical PRIMARY views; ordinary Native retains
    // PRIMARY/SECONDARY. A flat capture or an extra local-player view is excluded.
    if (count == 1 && stereo.pass == 1) { return static_cast<unsigned>(stereo.eye); }
    if (count == 2 && stereo.pass == stereo.eye+1) { return static_cast<unsigned>(stereo.eye); }
    return {};
}

struct ObjectHeader {
    uintptr_t vtable{};
    uint32_t flags{};
    int32_t index{};
    uintptr_t uclass{};
    int32_t name{}, number{};
    uintptr_t outer{};
};
static_assert(sizeof(ObjectHeader) == 0x28);

inline bool load_object(const Memory& m, uintptr_t p, uintptr_t base, size_t size, ObjectHeader& out) {
    return pointer(p) && m.load(p, out) && in_module(out.vtable, sizeof(uintptr_t), base, size) &&
        out.index >= 0 && out.index < 4000000 && pointer(out.uclass) &&
        out.name > 0 && out.number >= 0 && !(out.flags & 0x00018000u);
}

enum class Match { other, letterbox, unreadable };
inline Match classify(const Memory& m, uintptr_t material, const Names& names, uintptr_t base, size_t size) {
    ObjectHeader instance{}, cls{};
    if (!load_object(m, material, base, size, instance) ||
        !load_object(m, instance.uclass, base, size, cls)) { return Match::unreadable; }
    if (cls.name != names.dynamic_class || cls.number != 0) { return Match::other; }
    uintptr_t parent{};
    ObjectHeader asset{}, asset_class{};
    if (!m.load(material+0x128, parent) || !load_object(m, parent, base, size, asset) ||
        !load_object(m, asset.uclass, base, size, asset_class)) { return Match::unreadable; }
    return asset.name == names.parent && asset.number == 0 &&
        asset_class.name == names.material_class && asset_class.number == 0 ? Match::letterbox : Match::other;
}

// Return a complete replacement for the caller's inline value, without touching
// UObject ownership or the game's stored blendables. Heap/unknown layouts pass through.
inline std::optional<Chain> filtered_chain(const Memory& m, const Chain& original, int32_t location,
    const Names& names, uintptr_t base, size_t size) {
    if (location != 0 || !names.valid() || original.heap || original.capacity != 10 ||
        original.count < 1 || original.count > 10) { return {}; }
    auto result = original;
    int32_t kept{};
    for (int32_t i = 0; i < original.count; ++i) {
        const auto match = classify(m, original.elements[i], names, base, size);
        if (match == Match::unreadable) { return {}; }
        if (match != Match::letterbox) { result.elements[kept++] = original.elements[i]; }
    }
    if (kept == original.count) { return {}; }
    for (int32_t i = kept; i < original.count; ++i) { result.elements[i] = 0; }
    result.count = kept;
    return result;
}

template<class Original, class Enabled, class Apply>
Chain* dispatch(Chain* output, const void* view, int32_t location,
    Original&& original, Enabled&& enabled, Apply&& apply) {
    // Explicit sret ABI: RCX=caller-owned 0x60-byte buffer, RDX=View, R8d=location.
    auto* result = original(output, view, location);
    if (result == output && output && enabled()) { apply(output, view, location); }
    return result;
}
}
