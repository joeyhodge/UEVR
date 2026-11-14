#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <sdk/GearsEDayDiscovery.hpp>
#include "GearsEDayNativeContracts.hpp"

namespace uevr::gears_native {
inline constexpr auto family_copy_signature = [] {
    // Build the scan prefix from the same contract as validation, including REX.
    constexpr size_t prefix_size = 0x1f;
    constexpr char hex[] = "0123456789ABCDEF";
    std::array<char, prefix_size * 3 + 1> result{};
    size_t cursor{};
    for (size_t i = 0; i < prefix_size; ++i) {
        if (family_copy_mask[i] == 0) { result[cursor++] = '?'; }
        else {
            result[cursor++] = hex[family_copy_code[i] >> 4];
            result[cursor++] = hex[family_copy_code[i] & 15];
        }
        result[cursor++] = ' ';
    }
    result[cursor - 1] = '\0';
    return result;
}();
inline constexpr size_t family_size = 0x198;
inline constexpr size_t interfaces_offset = 0x160;
inline constexpr size_t constrained_rect_offset = 0x380;
inline constexpr size_t unconstrained_rect_offset = 0x390;

struct alignas(16) FamilyStorage {
    std::array<uint8_t, 0x1000> bytes{};
};
static_assert(sizeof(FamilyStorage) >= family_size && alignof(FamilyStorage) >= 16);

inline std::unique_ptr<FamilyStorage> make_family_storage() {
    // Only the outer placement storage is ours. The verified native copy/dtor
    // allocate/free the member arrays themselves; destructor flags remain zero.
    return std::unique_ptr<FamilyStorage>{new (std::nothrow) FamilyStorage{}};
}

struct FamilyFunctions {
    uintptr_t copy{};
    uintptr_t deleting_destructor{};
    uintptr_t complete_destructor{};
    uintptr_t vtable{};
};

template<class FunctionSize>
std::optional<FamilyFunctions> family_functions(const sdk::discovery::Memory& memory,
    uintptr_t copy, uintptr_t base, size_t image_size, FunctionSize&& function_size)
{
    const auto contains = [&](uintptr_t address, size_t size) {
        return address >= base && address - base <= image_size && size <= image_size - (address - base);
    };
    const auto call = [&](uintptr_t address) -> std::optional<uintptr_t> {
        std::array<uint8_t, 5> bytes{};
        int32_t displacement{};
        if (!memory.load(address, bytes) || bytes[0] != 0xe8) { return {}; }
        std::memcpy(&displacement, bytes.data() + 1, 4);
        const auto target = sdk::discovery::relative_address(address + 5, displacement);
        return target && contains(*target, 1) ? target : std::nullopt;
    };
    using sdk::gears_eday::matches;
    if (!contains(copy, family_copy_code.size()) || function_size(copy) != family_copy_code.size() ||
        !matches(memory, copy, family_copy_code, family_copy_mask)) { return {}; }
    int32_t displacement{};
    if (!memory.load(copy + 0x0e, displacement)) { return {}; }
    const auto table = sdk::discovery::relative_address(copy + 0x12, displacement);
    uintptr_t deleting{};
    if (!table || !contains(*table, sizeof(uintptr_t)) || !memory.load(*table, deleting) ||
        !contains(deleting, family_delete_code.size()) || function_size(deleting) != family_delete_code.size() ||
        !matches(memory, deleting, family_delete_code, family_delete_mask)) { return {}; }
    const auto destroy = call(deleting + 0x0f);
    const auto views_copy = call(copy + 0x31);
    const auto all_views_copy = call(copy + 0x4c);
    if (!destroy || !views_copy || all_views_copy != views_copy ||
        !contains(*destroy, family_destroy_code.size()) || function_size(*destroy) != family_destroy_code.size() ||
        !matches(memory, *destroy, family_destroy_code, family_destroy_mask) ||
        !contains(*views_copy, view_array_copy_code.size()) || function_size(*views_copy) != view_array_copy_code.size() ||
        !matches(memory, *views_copy, view_array_copy_code, view_array_copy_mask)) { return {}; }
    int32_t destroy_displacement{};
    if (!memory.load(*destroy + 9, destroy_displacement) ||
        sdk::discovery::relative_address(*destroy + 0x0d, destroy_displacement) != table) { return {}; }
    return FamilyFunctions{copy, deleting, *destroy, *table};
}

inline bool singleton_layout(const sdk::discovery::Memory& memory, uintptr_t constructor) {
    return sdk::gears_eday::view_constructor(memory, constructor) &&
        sdk::gears_eday::matches(memory, constructor + 0xc4, view_rects_code, view_rects_mask) &&
        sdk::gears_eday::matches(memory, constructor + 0x214, view_singleton_code, view_singleton_mask);
}

inline bool renderer_layout(const sdk::discovery::Memory& memory, uintptr_t renderer, size_t stolen_size) {
    return stolen_size > 0 && stolen_size <= 0x2a &&
        renderer <= (std::numeric_limits<uintptr_t>::max)() - 0x60 &&
        sdk::gears_eday::matches(memory, renderer + 0x2a, renderer_array_code, renderer_array_mask);
}

inline bool fresh_family(const sdk::discovery::Memory& memory, uintptr_t family, uintptr_t vtable) {
    std::array<uint8_t, family_size> bytes{};
    if (!memory.load(family, bytes)) { return false; }
    const auto pointer_at = [&](size_t offset) {
        uintptr_t value{};
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    };
    // Owned upscalers and SceneRenderer must not be recycled from a prior render.
    return pointer_at(0) == vtable && bytes[0xb0] <= 1 && pointer_at(0x158) == 0 &&
        pointer_at(0x168) == 0 && pointer_at(0x170) == 0 && pointer_at(0x178) == 0;
}

using Rect = std::array<int32_t, 4>;
struct ViewRects {
    Rect constrained{}, unconstrained{};
    bool operator==(const ViewRects&) const = default;
};

inline bool read_rects(const sdk::discovery::Memory& memory, uintptr_t view, ViewRects& out) {
    return sdk::gears_eday::pointer(view) &&
        memory.load(view + constrained_rect_offset, out.constrained) &&
        memory.load(view + unconstrained_rect_offset, out.unconstrained);
}

inline std::optional<ViewRects> rebase_right(const ViewRects& left, const ViewRects& right,
    uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) { return {}; }
    const auto w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
    if (left.unconstrained != Rect{0, 0, w, h} || right.unconstrained != Rect{w, 0, 2 * w, h} ||
        left.constrained[0] < 0 || left.constrained[1] < 0 ||
        left.constrained[2] <= left.constrained[0] || left.constrained[2] > w ||
        left.constrained[3] <= left.constrained[1] || left.constrained[3] > h) { return {}; }
    auto next = right;
    for (const auto i : {0, 2}) {
        if (next.constrained[i] < w || next.constrained[i] > 2 * w) { return {}; }
        next.constrained[i] -= w;
        next.unconstrained[i] -= w;
    }
    return next == left ? std::optional{next} : std::nullopt;
}

// Leave ordinary/fail-closed rendering packed. Only the validated linked
// transaction may temporarily move the right eye into its single-eye target.
class RectOverride {
public:
    using Writer = bool(*)(void*, uintptr_t, const ViewRects&);
    RectOverride() = default;
    RectOverride(const RectOverride&) = delete;
    RectOverride& operator=(const RectOverride&) = delete;
    ~RectOverride() { restore(); }

    bool initialize(const sdk::discovery::Memory& memory, uintptr_t left, uintptr_t right,
        const ViewRects& left_metadata, const ViewRects& right_metadata,
        uint32_t width, uint32_t height, void* context, Writer writer) {
        if (m_active || !writer || left == right) { return false; }
        ViewRects l{}, r{};
        if (!read_rects(memory, left, l) || !read_rects(memory, right, r) ||
            l != left_metadata || r != right_metadata) { return false; }
        const auto next = rebase_right(l, r, width, height);
        if (!next) { return false; }
        m_memory = memory; m_context = context; m_writer = writer; m_view = right; m_saved = r;
        m_active = true;
        if (!writer(context, right, *next) || !read_rects(memory, right, r) || r != *next) {
            restore();
            return false;
        }
        return true;
    }
    bool restore() {
        if (!m_active) { return true; }
        ViewRects current{};
        if (!m_writer(m_context, m_view, m_saved) ||
            !read_rects(m_memory, m_view, current) || current != m_saved) { return false; }
        m_active = false;
        return true;
    }
private:
    sdk::discovery::Memory m_memory{};
    void* m_context{};
    Writer m_writer{};
    uintptr_t m_view{};
    ViewRects m_saved{};
    bool m_active{};
};
}
