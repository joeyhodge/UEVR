#pragma once

#include <algorithm>
#include <sdk/GalacticRacerRuntime.hpp>
#include "GalacticRacerNativeFixCode.hpp"

namespace uevr::swgr_native {

using sdk::galactic_racer::code_matches;
inline constexpr const char* family_copy_signature =
    "41 57 41 56 56 57 53 48 83 EC 40 49 89 D6 48 89 CB 48 8D 05 ? ? ? ? 48 89 01";
inline constexpr size_t view_constructor_size = 0x1e59;
inline constexpr size_t stereo_pass_offset = 0xdf0;
inline constexpr size_t primary_index_offset = 0xdf8;
inline constexpr size_t unscaled_rect_offset = 0x390;
inline constexpr size_t unconstrained_rect_offset = 0x3b0;

inline bool in_module(uintptr_t p, size_t n, uintptr_t base, size_t size) {
    return p >= base && p - base <= size && n <= size - (p - base);
}

inline std::optional<uintptr_t> edge(const sdk::discovery::Memory& m, uintptr_t ip,
    uintptr_t base, size_t size, uint8_t opcode = 0xe8) {
    std::array<uint8_t, 5> bytes{};
    int32_t displacement{};
    if (!in_module(ip, bytes.size(), base, size) || !m.load(ip, bytes) || bytes[0] != opcode) { return {}; }
    std::memcpy(&displacement, bytes.data() + 1, sizeof(displacement));
    const auto target = sdk::discovery::relative_address(ip + bytes.size(), displacement);
    if (!target || !in_module(*target, 1, base, size) || !m.executable ||
        !m.executable(m.context, *target, 1)) { return {}; }
    return target;
}

inline std::optional<uintptr_t> vtable_lea(const sdk::discovery::Memory& m,
    uintptr_t ip, uintptr_t base, size_t size) {
    std::array<uint8_t, 7> bytes{};
    int32_t displacement{};
    if (!in_module(ip, bytes.size(), base, size) || !m.load(ip, bytes) ||
        bytes[0] != 0x48 || bytes[1] != 0x8d || bytes[2] != 0x05) { return {}; }
    std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
    const auto table = sdk::discovery::relative_address(ip + bytes.size(), displacement);
    if (!table || (*table & 7) != 0 || !in_module(*table, sizeof(uintptr_t), base, size)) { return {}; }
    return table;
}

struct FamilyFunctions {
    uintptr_t copy{}, table{}, scalar_destructor{};
};

// Read-only proof: a base-family copy owns its arrays/refcounts but borrows the
// four upscaler interfaces. The context destructor must never delete these views.
template<class FunctionSize>
std::optional<FamilyFunctions> family_functions(const sdk::discovery::Memory& m,
    uintptr_t copy, size_t unwind_size, uintptr_t base, size_t size, FunctionSize&& function_size) {
    if (unwind_size != family_copy_code.size() || !in_module(copy, unwind_size, base, size) ||
        !code_matches(m, copy, family_copy_code, family_copy_mask)) { return {}; }
    const auto table = vtable_lea(m, copy + 0x11, base, size);
    uintptr_t scalar{};
    if (!table || !m.load(*table, scalar) || !in_module(scalar, scalar_destructor_code.size(), base, size) ||
        function_size(scalar) != scalar_destructor_code.size() ||
        !code_matches(m, scalar, scalar_destructor_code, scalar_destructor_mask)) { return {}; }
    const auto complete = edge(m, scalar + 0xb, base, size);
    if (!complete || !in_module(*complete, complete_destructor_code.size(), base, size) ||
        function_size(*complete) != complete_destructor_code.size() ||
        !code_matches(m, *complete, complete_destructor_code, complete_destructor_mask) ||
        vtable_lea(m, *complete + 8, base, size) != table || !edge(m, scalar + 0x1c, base, size)) { return {}; }

    const auto allocator = edge(m, copy + 0x386, base, size);
    const auto memcpy_thunk = edge(m, copy + 0x398, base, size);
    const auto origins = edge(m, copy + 0x1cf, base, size);
    const auto flags = edge(m, copy + 0x353, base, size);
    if (!allocator || !memcpy_thunk || !origins || !flags ||
        !in_module(*allocator, array_allocate_code.size(), base, size) ||
        !in_module(*origins, origins_allocate_code.size(), base, size) ||
        !in_module(*flags, flags_allocate_code.size(), base, size) ||
        function_size(*allocator) != array_allocate_code.size() ||
        function_size(*origins) != origins_allocate_code.size() ||
        function_size(*flags) != flags_allocate_code.size() ||
        !code_matches(m, *allocator, array_allocate_code, array_allocate_mask) ||
        !code_matches(m, *origins, origins_allocate_code, origins_allocate_mask) ||
        !code_matches(m, *flags, flags_allocate_code, flags_allocate_mask)) { return {}; }
    for (auto off : {0x22, 0x56}) { if (!edge(m, *allocator + off, base, size)) { return {}; } }
    for (auto off : {0x61, 0x74}) { if (!edge(m, *origins + off, base, size)) { return {}; } }
    for (auto off : {0x60, 0x72, 0x7b, 0x8b}) { if (!edge(m, *flags + off, base, size)) { return {}; } }
    for (auto off : {0x2d8, 0x315, 0x3c4, 0x405}) {
        if (edge(m, copy + off, base, size) != allocator) { return {}; }
    }
    for (auto off : {0x1ed, 0x2a9, 0x326, 0x3d6}) {
        if (edge(m, copy + off, base, size) != memcpy_thunk) { return {}; }
    }
    for (auto off : {0x2e6, 0x413}) {
        const auto shared = edge(m, copy + off, base, size);
        if (!shared || !in_module(*shared, shared_copy_code.size(), base, size) ||
            !code_matches(m, *shared, shared_copy_code, shared_copy_mask)) { return {}; }
    }
    const auto destroy_extensions = edge(m, *complete + 0x5f, base, size);
    const auto destroy_view_extensions = edge(m, *complete + 0x9d, base, size);
    if (!destroy_extensions || !destroy_view_extensions ||
        !in_module(*destroy_extensions, extensions_destroy_code.size(), base, size) ||
        !in_module(*destroy_view_extensions, view_extensions_destroy_code.size(), base, size) ||
        !code_matches(m, *destroy_extensions, extensions_destroy_code, extensions_destroy_mask) ||
        !code_matches(m, *destroy_view_extensions, view_extensions_destroy_code, view_extensions_destroy_mask) ||
        !edge(m, *destroy_extensions + 0x1d, base, size) ||
        edge(m, *destroy_extensions + 0x1d, base, size) !=
            edge(m, *destroy_view_extensions + 0x16, base, size)) { return {}; }
    const auto release = edge(m, *destroy_extensions + 0x1d, base, size);
    constexpr std::array<uint8_t, 17> release_prefix{
        0x56,0x48,0x83,0xec,0x20,0x48,0x85,0xc9,0x74,0x09,0x48,0x89,0xce,0xf0,0xff,0x49,0x08};
    constexpr auto release_mask = [] { std::array<uint8_t, 17> mask{}; mask.fill(0xff); return mask; }();
    if (!release || !in_module(*release, release_prefix.size(), base, size) ||
        !code_matches(m, *release, release_prefix, release_mask)) { return {}; }
    const auto free = edge(m, *complete + 0xd2, base, size, 0xe9);
    if (!free) { return {}; }
    for (auto off : {0xed, 0xf4, 0xfb, 0x132, 0x13c, 0x146}) {
        if (edge(m, *complete + off, base, size) != free) { return {}; }
    }
    return FamilyFunctions{copy, *table, scalar};
}

// Inspect blocks beyond the inline hook's prologue, not a guessed FSceneView
// layout from another UE5.7 title. No view or InitOptions fields are written here.
inline bool view_layout(const sdk::discovery::Memory& m, uintptr_t ctor,
    size_t unwind_size, uintptr_t base, size_t size) {
    return unwind_size == view_constructor_size && in_module(ctor, unwind_size, base, size) &&
        code_matches(m, ctor + 0x74, view_arguments_code, view_arguments_mask) &&
        code_matches(m, ctor + 0x96, view_family_code, view_family_mask) &&
        code_matches(m, ctor + 0x2e4, view_rects_code, view_rects_mask) &&
        code_matches(m, ctor + 0x4ce, view_stereo_code, view_stereo_mask) &&
        code_matches(m, ctor + 0x132d, view_primary_code, view_primary_mask);
}

using Rect = std::array<int32_t, 4>;
struct ViewRects {
    Rect constrained{}, unconstrained{};
    bool operator==(const ViewRects&) const = default;
};

inline bool read_rects(const sdk::discovery::Memory& m, uintptr_t view, ViewRects& out) {
    return sdk::galactic_racer::pointer(view) &&
        m.load(view + unscaled_rect_offset, out.constrained) &&
        m.load(view + unconstrained_rect_offset, out.unconstrained);
}

inline std::optional<ViewRects> rebase_right(const ViewRects& left, const ViewRects& right,
    uint32_t width, uint32_t height) {
    if (!width || !height || width > 16384 || height > 16384) { return {}; }
    const auto w = static_cast<int32_t>(width), h = static_cast<int32_t>(height);
    if (left.unconstrained != Rect{0, 0, w, h} || right.unconstrained != Rect{w, 0, 2 * w, h} ||
        left.constrained[0] < 0 || left.constrained[1] < 0 ||
        left.constrained[2] <= left.constrained[0] || left.constrained[2] > w ||
        left.constrained[3] <= left.constrained[1] || left.constrained[3] > h) { return {}; }
    auto rebased = right;
    for (auto i : {0, 2}) {
        if (rebased.constrained[i] < w || rebased.constrained[i] > 2 * w) { return {}; }
        rebased.constrained[i] -= w; rebased.unconstrained[i] -= w;
    }
    if (rebased != left) { return {}; }
    return rebased;
}

// Construction/fallback keeps the original packed stereo rectangles. Only the
// validated linked render uses a zero-origin right-eye capture rectangle.
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
