#pragma once

#include <safetyhook.hpp>
#include "KtjLCloudOutput.hpp"
#include "KtjLMeshResourceContracts.hpp"

namespace uevr::ktjl::mesh {
struct AllocationPath {
    uintptr_t allocator, boundary, fallback, frame_allocator, vtable, destructor;
    size_t size;
};
// All four specializations share the relocating arena, but their constructors
// and cold branches differ. Resume each one's own frame-lifetime fallback.
inline constexpr std::array<AllocationPath, 4> allocation_paths{{
    {allocator_rva,   0x6A263D,  fallback_branch_rva, frame_allocator_rva,   0x698D350, deleting_destructor_rva,   24},
    {allocator_b_rva, 0x638501C, 0x638500F,           frame_allocator_b_rva, 0x881FFF8, deleting_destructor_b_rva, 24},
    {allocator_c_rva, 0x6385096, 0x6385089,           frame_allocator_c_rva, 0x69851E0, deleting_destructor_c_rva, 16},
    {allocator_d_rva, 0x638510E, 0x6385101,           frame_allocator_d_rva, 0x881FFF0, deleting_destructor_d_rva, 24}
}};
inline constexpr uintptr_t work_rva = 0x2875A20;
inline constexpr size_t collector_offset = 0xC8;
inline constexpr size_t frame_pointer_offset = 0x100, caller_offset = 0x1B8;
inline constexpr std::array<uintptr_t, 3> callers{0x762767, 0x7629A8, 0x762B41};

inline bool is_gather_call(const sdk::discovery::Memory& m, uintptr_t base, const safetyhook::Context& c) {
    if (!sdk::ktjl::pointer(c.rsp) || c.rsp > 0x0000800000000000ULL - caller_offset - 16 ||
        c.rbp != c.rsp + 8 + frame_pointer_offset) { return false; }
    uintptr_t caller{};
    return m.load(c.rsp, caller) && caller == base + 0x75EF57;
}

inline bool validate_code(const sdk::discovery::Memory& m, uintptr_t base) {
    if (!sdk::ktjl::validated_image(m, base)) { return false; }
    for (const auto& e : code_evidence) {
        std::array<uint8_t, 512> bytes{};
        if (!m.read || !m.executable || e.bytes.size() > bytes.size() ||
            !m.executable(m.context, base + e.rva, e.bytes.size()) ||
            !m.read(m.context, base + e.rva, bytes.data(), e.bytes.size()) ||
            std::memcmp(bytes.data(), e.bytes.data(), e.bytes.size()) != 0) { return false; }
    }
    for (const auto& path : allocation_paths) {
        uintptr_t destructor{};
        if (!m.load(base + path.vtable, destructor) || destructor != base + path.destructor) { return false; }
    }
    return true;
}

inline bool paired_collector(const sdk::discovery::Memory& m, uintptr_t base,
                             uintptr_t collector, uintptr_t view) {
    if (!sdk::ktjl::pointer(collector) || collector < collector_offset) { return false; }
    uint32_t tls{};
    uint8_t gathering{};
    const auto pair = cloud_output::read_pair(m, base, view);
    return pair && collector == pair->renderer + collector_offset &&
        m.load(collector + 0x2E0, gathering) && gathering == 1 &&
        m.load(collector + 0x2DC, tls) && tls != UINT32_MAX;
}

inline bool select_serial(const sdk::discovery::Memory& m, uintptr_t base,
                          const safetyhook::Context& c) {
    if (!is_gather_call(m, base, c) || c.rdx != c.rbp + 0x28) { return false; }
    std::array<uintptr_t, 2> work{};
    uintptr_t caller{};
    if (!m.load(c.rdx, work) || work[0] != base + work_rva || work[1] != c.rbp - 0x28 ||
        !m.load(c.rsp + 8 + caller_offset, caller) ||
        std::none_of(callers.begin(), callers.end(), [&](uintptr_t rva) { return caller == base + rva; })) { return false; }
    return paired_collector(m, base, c.r12, c.rsi);
}

// A render-thread scope surrounds only this ParallelFor and its collector join.
// No engine TLS, arena ownership, destructor or task/fence is rewritten.
template<size_t Capacity = 16>
class GatherScope {
    static_assert(Capacity > 0);
    struct Entry { uintptr_t stack{}, collector{}; };
    std::array<Entry, Capacity> m_entries{};
    size_t m_size{}, m_overflow{};

public:
    void begin(uintptr_t stack, uintptr_t collector) {
        // A later sibling also removes a scope abandoned by stack unwinding.
        while (m_size && stack >= m_entries[m_size - 1].stack) {
            --m_size;
            m_overflow = 0;
        }
        if (m_size == Capacity) { ++m_overflow; }
        else { m_entries[m_size++] = {stack, collector}; }
    }

    void end(uintptr_t stack) {
        if (!m_size) { return; }
        if (m_overflow && stack < m_entries[m_size - 1].stack) { --m_overflow; return; }
        if (m_entries[m_size - 1].stack == stack) { --m_size; m_overflow = 0; }
    }

    bool use_frame_allocator(const sdk::discovery::Memory& m, uintptr_t base,
                             const safetyhook::Context& c) const {
        if (!m_size || c.rsp >= m_entries[m_size - 1].stack || !sdk::ktjl::pointer(c.r8)) { return false; }
        if (!m_overflow) {
            return m_entries[m_size - 1].collector != 0 && c.rbx == m_entries[m_size - 1].collector;
        }
        // Deep nesting does not silently drop the lifetime repair. It uses the
        // same bounded structural validation instead of an unbounded TLS stack.
        if (!sdk::ktjl::pointer(c.rbx) || c.rbx < collector_offset) { return false; }
        fog::Header views{};
        return m.load(c.rbx - collector_offset + fog::views_offset, views) &&
            paired_collector(m, base, c.rbx, views.data);
    }
};

inline void use_serial(safetyhook::Context& c) { c.r8 |= uintptr_t{1}; }
template<size_t Path, size_t Capacity>
inline bool redirect_allocation(const sdk::discovery::Memory& m, uintptr_t base,
                                const GatherScope<Capacity>& scope, safetyhook::Context& c) {
    static_assert(Path < allocation_paths.size());
    if (!scope.use_frame_allocator(m, base, c)) { return false; }
    c.rip = base + allocation_paths[Path].fallback;
    return true;
}

inline bool keep_native_fallback(bool target, bool native, bool native_fix) {
    return target && native && !native_fix;
}
}
