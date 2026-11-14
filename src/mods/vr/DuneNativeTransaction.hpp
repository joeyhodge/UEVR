#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace uevr::dune_native {

inline constexpr size_t maximum_constructor_bytes = 0x2000;
inline constexpr size_t family_offset = 0x10;
inline constexpr size_t state_offset = 0x18;
inline constexpr size_t pass_offset = 0x13A0;
inline constexpr size_t stereo_flags_offset = 0x19CC;

// Independently prove the fields used below in the resolved constructor. Moved
// functions are fine; changed layouts fail closed instead of inheriting RVAs.
inline constexpr auto owner_code = std::to_array<uint8_t>({
    0x48,0x8B,0x82,0x40,0x01,0,0, 0x48,0x8B,0xD9, 0x48,0x89,0x41,0x10,
    0x48,0x8B,0x82,0x48,0x01,0,0, 0x48,0x89,0x41,0x18});
inline constexpr auto pass_code = std::to_array<uint8_t>({
    0x8B,0x87,0x98,0x01,0,0, 0x48,0x8D,0x8B,0x68,0x14,0,0,
    0x89,0x83,0xA0,0x13,0,0, 0x8B,0x87,0x9C,0x01,0,0,
    0xC7,0x83,0xA8,0x13,0,0,0xFF,0xFF,0xFF,0xFF});
inline constexpr auto index_code = std::to_array<uint8_t>({0x89,0x83,0xA4,0x13,0,0});
inline constexpr auto instanced_code = std::to_array<uint8_t>({0x88,0x8B,0xCC,0x19,0,0});
inline constexpr auto multiview_code = std::to_array<uint8_t>({0x88,0x83,0xCF,0x19,0,0});
inline constexpr auto primary_code = std::to_array<uint8_t>({
    0x8B,0x40,0x10,0x48,0x8B,0xCB,0x89,0x83,0xA8,0x13,0,0});

template <size_t N>
std::optional<size_t> unique_offset(std::span<const uint8_t> code, const std::array<uint8_t, N>& pattern) {
    const auto found = std::search(code.begin(), code.end(), pattern.begin(), pattern.end());
    if (found == code.end() || std::search(found + 1, code.end(), pattern.begin(), pattern.end()) != code.end()) { return {}; }
    return static_cast<size_t>(found - code.begin());
}

inline bool valid_constructor(std::span<const uint8_t> code) {
    if (code.size() > maximum_constructor_bytes) { return false; }
    const auto owner = unique_offset(code, owner_code), pass = unique_offset(code, pass_code);
    const auto index = unique_offset(code, index_code), instanced = unique_offset(code, instanced_code);
    const auto multiview = unique_offset(code, multiview_code), primary = unique_offset(code, primary_code);
    return owner && pass && index && instanced && multiview && primary &&
        *owner + owner_code.size() <= *pass && *pass + pass_code.size() <= *index &&
        *index + index_code.size() <= *instanced && *instanced + instanced_code.size() <= *multiview &&
        *multiview + multiview_code.size() <= *primary;
}

struct Eye {
    uintptr_t family{}, state{};
    uint32_t pass{};
    int32_t stereo_index{}, primary_index{};
    std::array<uint8_t, 4> flags{};
    bool operator==(const Eye&) const = default;
};

template <typename Read>
std::optional<Eye> read_eye(uintptr_t view, Read& read) {
    if (view < 0x10000 || (view & 7) != 0 ||
        view > std::numeric_limits<uintptr_t>::max() - stereo_flags_offset - 4) { return {}; }
    std::array<uintptr_t, 2> owner{};
    std::array<int32_t, 3> stereo{};
    Eye eye{};
    if (!read(view + family_offset, owner.data(), sizeof(owner)) ||
        !read(view + pass_offset, stereo.data(), sizeof(stereo)) ||
        !read(view + stereo_flags_offset, eye.flags.data(), eye.flags.size())) { return {}; }
    eye.family = owner[0]; eye.state = owner[1];
    eye.pass = static_cast<uint32_t>(stereo[0]);
    eye.stereo_index = stereo[1]; eye.primary_index = stereo[2];
    if (std::any_of(eye.flags.begin(), eye.flags.end(), [](uint8_t value) { return value > 1; }) ||
        eye.flags[0] != 0 || eye.flags[3] != 0) { return {}; }
    return eye;
}

template <typename Read, typename Write>
class SingletonPrimary {
public:
    SingletonPrimary(Read read, Write write) : m_read{read}, m_write{write} {}
    SingletonPrimary(const SingletonPrimary&) = delete;
    SingletonPrimary& operator=(const SingletonPrimary&) = delete;
    ~SingletonPrimary() { restore(); }

    // Preparation is read-only and precedes BOTH renderer calls. Rejected
    // preparation leaves an intact engine pair for learning/transition fallback.
    bool prepare(bool validated, uintptr_t family, uintptr_t left, uintptr_t right,
        uintptr_t left_state, uintptr_t right_state, uint64_t generation) {
        if (m_active) { return false; }
        m_prepared = false;
        if (!validated || !generation || family < 0x10000 || left == right ||
            !left_state || !right_state || left_state == right_state || !family_views(family, left, right, 2)) { return false; }
        const auto l = read_eye(left, m_read), r = read_eye(right, m_read);
        if (!l || !r || l->family != family || r->family != family ||
            l->state != left_state || r->state != right_state || l->pass != 1 || r->pass != 2 ||
            l->stereo_index != 0 || r->stereo_index != 1 || l->primary_index != 0 || r->primary_index != 0) { return false; }
        m_family = family; m_right = right; m_saved = *r; m_generation = generation; m_prepared = true;
        return true;
    }

    // Change only StereoPass, only for the right singleton's synchronous copy
    // into FViewInfo. State, exposure, index, flags and projection stay authored.
    bool apply(uint64_t current_generation) {
        if (m_active || !m_prepared || m_generation != current_generation ||
            !family_views(m_family, m_right, 0, 1)) { return false; }
        const auto before = read_eye(m_right, m_read);
        if (!before || *before != m_saved) { return false; }
        m_active = true;
        const uint32_t primary = 1;
        auto expected = m_saved; expected.pass = primary;
        if (!m_write(m_right + pass_offset, &primary, sizeof(primary)) || read_eye(m_right, m_read) != expected) {
            restore();
            return false;
        }
        return true;
    }

    bool restore() {
        if (!m_active) { return true; }
        uint32_t restored{};
        if (!m_write(m_right + pass_offset, &m_saved.pass, sizeof(m_saved.pass)) ||
            !m_read(m_right + pass_offset, &restored, sizeof(restored)) || restored != m_saved.pass) { return false; }
        m_active = false;
        return true;
    }

private:
    bool family_views(uintptr_t family, uintptr_t first, uintptr_t second, int32_t count) {
        if ((family & 7) != 0 || family > std::numeric_limits<uintptr_t>::max() - 0x18) { return false; }
        uintptr_t data{};
        std::array<int32_t, 2> shape{};
        std::array<uintptr_t, 2> views{};
        return m_read(family + 8, &data, sizeof(data)) && m_read(family + 0x10, shape.data(), sizeof(shape)) &&
            shape[0] == count && shape[1] >= count && shape[1] <= 1024 && data >= 0x10000 && (data & 7) == 0 &&
            m_read(data, views.data(), static_cast<size_t>(count) * sizeof(uintptr_t)) &&
            views[0] == first && (count == 1 || views[1] == second);
    }

    Read m_read;
    Write m_write;
    Eye m_saved{};
    uintptr_t m_family{}, m_right{};
    uint64_t m_generation{};
    bool m_prepared{}, m_active{};
};

} // namespace uevr::dune_native
