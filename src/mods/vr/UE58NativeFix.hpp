#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>

#include <sdk/TheSinkingCity2Native.hpp>
#include "UE58NativeContracts.hpp"

namespace uevr::ue58_native {

inline constexpr size_t family_size = 0x1a8;
inline constexpr size_t interfaces_offset = 0x168;
using Interfaces = std::array<uintptr_t, 5>;
using Eye = sdk::sinking_city_2::native::Eye;
using EyeOverride = sdk::sinking_city_2::native::EyeOverride;
inline constexpr size_t constrained_offset = 0x390, unconstrained_offset = 0x3a0;
inline constexpr size_t pass_offset = 0xe70, eye_index_offset = 0xe74, primary_index_offset = 0xe78;
static_assert(constrained_offset == sdk::sinking_city_2::native::constrained_view_offset &&
    unconstrained_offset == sdk::sinking_city_2::native::unconstrained_view_offset &&
    pass_offset == sdk::sinking_city_2::native::pass_offset &&
    primary_index_offset == sdk::sinking_city_2::native::primary_index_offset);

inline bool needs_guarded_fallback(std::wstring_view path) {
    const auto separator = path.find_last_of(L"/\\");
    path = path.substr(separator == path.npos ? 0 : separator + 1);
    constexpr std::wstring_view name = L"slasher-win64-shipping.exe";
    if (path.size() != name.size()) { return false; }
    for (size_t i = 0; i < path.size(); ++i) {
        const auto c = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (c != name[i]) { return false; }
    }
    return true;
}

template<size_t N>
bool matches(const sdk::discovery::Memory& memory, uintptr_t address,
    const std::array<uint8_t, N>& code, const std::array<uint8_t, N>& mask) {
    return sdk::sinking_city_2::native::matches(memory, address, code, mask);
}

inline bool view_layout(const sdk::discovery::Memory& memory, uintptr_t constructor) {
    using namespace contract;
    return sdk::discovery::relative_address(constructor, 0x5d2) &&
        matches(memory, constructor + 0x30, view_identity_code, view_identity_mask) &&
        matches(memory, constructor + 0x3b9, view_rects_code, view_rects_mask) &&
        matches(memory, constructor + 0x49f, view_projection_code, view_projection_mask) &&
        matches(memory, constructor + 0x547, view_stereo_code, view_stereo_mask);
}

inline bool renderer_layout(const sdk::discovery::Memory& memory, uintptr_t renderer) {
    using namespace contract;
    return sdk::discovery::relative_address(renderer, 0x1b4) &&
        matches(memory, renderer + 0x2a, renderer_span_code, renderer_span_mask) &&
        matches(memory, renderer + 0x19c, renderer_frame_code, renderer_frame_mask);
}

struct FamilyFunctions { uintptr_t copy{}, destroy{}, vtable{}; };

template<class FunctionSize>
std::optional<FamilyFunctions> family_functions(const sdk::discovery::Memory& memory,
    uintptr_t copy, uintptr_t base, size_t image_size, FunctionSize&& function_size) {
    using namespace contract;
    const auto contains = [&](uintptr_t p, size_t n) {
        return base >= 0x10000 && p >= base && p - base < image_size && n <= image_size - (p - base);
    };
    if (!contains(copy, family_copy_code.size()) || function_size(copy) != family_copy_code.size() ||
        !matches(memory, copy, family_copy_code, family_copy_mask)) { return {}; }
    int32_t displacement{};
    if (!memory.load(copy + 0x2b, displacement)) { return {}; }
    const auto table = sdk::discovery::relative_address(copy + 0x2f, displacement);
    uintptr_t deleting{};
    if (!table || !contains(*table, sizeof(deleting)) || !memory.load(*table, deleting) ||
        !contains(deleting, family_delete_code.size()) || function_size(deleting) != family_delete_code.size() ||
        !matches(memory, deleting, family_delete_code, family_delete_mask) ||
        !memory.load(deleting + 0x10, displacement)) { return {}; }
    const auto destroy = sdk::discovery::relative_address(deleting + 0x14, displacement);
    if (!destroy || !contains(*destroy, family_destroy_code.size()) ||
        function_size(*destroy) != family_destroy_code.size() ||
        !matches(memory, *destroy, family_destroy_code, family_destroy_mask) ||
        !memory.load(*destroy + 9, displacement) ||
        sdk::discovery::relative_address(*destroy + 0xd, displacement) != table) { return {}; }
    return FamilyFunctions{copy, *destroy, *table};
}

inline bool fresh_interfaces(const Interfaces& values) {
    return std::all_of(values.begin() + 1, values.end(), [](uintptr_t p) { return p == 0; });
}

inline bool retain_shared_interfaces(const Interfaces& source, const Interfaces& clone, Interfaces& borrowed) {
    bool independent = true;
    for (size_t i = 0; i < clone.size(); ++i) {
        if (!clone[i]) { continue; }
        for (size_t j = 0; j < source.size(); ++j) {
            if (clone[i] != source[j]) { continue; }
            borrowed[i] = clone[i];
            if (i != 0 || j != 0) { independent = false; }
        }
        for (size_t j = 0; j < i; ++j) {
            if (clone[i] == clone[j]) { borrowed[i] = clone[i]; independent = false; }
        }
    }
    return independent;
}

struct ViewArray { uintptr_t data{}; int32_t count{}, capacity{}; };
static_assert(sizeof(ViewArray) == 16);

// Native construction retains shared-ref members and deep-copies every array.
// Only the base destructor is permitted: the context destructor deletes views.
class FamilyClone {
public:
    FamilyClone() = default;
    FamilyClone(const FamilyClone&) = delete;
    FamilyClone& operator=(const FamilyClone&) = delete;
    ~FamilyClone() {
        if (!m_constructed) { return; }
        for (const size_t offset : {size_t{8}, size_t{0x18}}) {
            ViewArray array{};
            std::memcpy(&array, m_storage.data() + offset, sizeof(array));
            if (array.data && (array.data == m_source_arrays[0] || array.data == m_source_arrays[1])) {
                std::memset(m_storage.data() + offset, 0, sizeof(array));
            }
        }
        for (size_t i = 0; i < m_borrowed.size(); ++i) {
            auto* slot = m_storage.data() + interfaces_offset + i * sizeof(uintptr_t);
            uintptr_t value{};
            std::memcpy(&value, slot, sizeof(value));
            if (value && value == m_borrowed[i]) { std::memset(slot, 0, sizeof(value)); }
        }
        reinterpret_cast<void(*)(void*)>(m_functions.destroy)(m_storage.data());
    }
    uintptr_t address() { return m_constructed ? reinterpret_cast<uintptr_t>(m_storage.data()) : 0; }
    bool original_render_safe() const { return m_original_render_safe; }
    bool initialize(const sdk::discovery::Memory& memory, uintptr_t source,
        uintptr_t expected_vtable, const FamilyFunctions& functions, const char*& reason) {
        reason = "native family copy contract is unavailable";
        if (m_constructed || !functions.copy || !functions.destroy || !functions.vtable) { return false; }
        std::array<uint8_t, family_size> bytes{};
        if (!source || !memory.load(source, bytes)) { return false; }
        uintptr_t table{}, renderer{};
        std::memcpy(&table, bytes.data(), sizeof(table));
        std::memcpy(&renderer, bytes.data() + 0x160, sizeof(renderer));
        std::memcpy(m_borrowed.data(), bytes.data() + interfaces_offset, sizeof(m_borrowed));
        m_original_render_safe = renderer == 0 && fresh_interfaces(m_borrowed);
        if (table != expected_vtable || !m_original_render_safe || bytes[0xb8] != 0 ||
            bytes[0xb0] > 1 || bytes[0xb7] > 1) {
            reason = "family is already consumed or not an original viewport family"; return false;
        }
        ViewArray views{}, all{};
        std::array<uintptr_t, 2> entries{};
        std::memcpy(&views, bytes.data() + 8, sizeof(views));
        std::memcpy(&all, bytes.data() + 0x18, sizeof(all));
        if (views.count != 2 || views.capacity < 2 || views.capacity > 16 || !views.data ||
            all.count != 0 || all.capacity < 0 || all.capacity > 32 ||
            !memory.load(views.data, entries) || !entries[0] || !entries[1] || entries[0] == entries[1]) {
            reason = "family is not an unconsumed two-eye input"; return false;
        }
        m_functions = functions;
        m_source_arrays = {views.data, all.data};
        auto* result = reinterpret_cast<void*(*)(void*, const void*)>(functions.copy)(
            m_storage.data(), reinterpret_cast<const void*>(source));
        m_constructed = true;
        ViewArray copy_views{}, copy_all{};
        Interfaces copied_interfaces{};
        std::memcpy(&table, m_storage.data(), sizeof(table));
        std::memcpy(&copy_views, m_storage.data() + 8, sizeof(copy_views));
        std::memcpy(&copy_all, m_storage.data() + 0x18, sizeof(copy_all));
        std::memcpy(copied_interfaces.data(), m_storage.data() + interfaces_offset, sizeof(copied_interfaces));
        std::array<uintptr_t, 2> copied{};
        const bool independent = copy_views.count == 2 && copy_views.capacity >= 2 && copy_views.capacity <= 16 &&
            copy_views.data && copy_views.data != views.data && copy_views.data != all.data &&
            memory.load(copy_views.data, copied) && copied == entries;
        if (result != m_storage.data() || table != functions.vtable || !independent ||
            copy_all.count != 0 || copy_all.data != 0 || copy_all.capacity != 0 ||
            copied_interfaces != m_borrowed ||
            std::memcmp(m_storage.data() + 0x30, bytes.data() + 0x30, 0x18) != 0) {
            reason = "native family copy did not retain independent storage and ownership"; return false;
        }
        // Additional family, not first; never pretend base placement storage is FViewFamilyInfo.
        m_storage[0xb0] = 1;
        m_storage[0xb7] = 0;
        reason = nullptr;
        return true;
    }
    bool finish(const sdk::discovery::Memory& memory, uintptr_t source) {
        Interfaces original{}, clone{};
        if (!m_constructed) { return false; }
        std::memcpy(clone.data(), m_storage.data() + interfaces_offset, sizeof(clone));
        if (!memory.load(source + interfaces_offset, original)) {
            m_borrowed = clone;
            return false;
        }
        return retain_shared_interfaces(original, clone, m_borrowed);
    }
private:
    alignas(16) std::array<uint8_t, family_size> m_storage{};
    Interfaces m_borrowed{};
    std::array<uintptr_t, 2> m_source_arrays{};
    FamilyFunctions m_functions{};
    bool m_constructed{};
    bool m_original_render_safe{true};
};

} // namespace uevr::ue58_native
