#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace uevr::swgr_panini {
inline std::optional<uint32_t> flags_offset(std::span<const uint8_t> code) {
    uint32_t offset{};
    if (code.size() >= 4 && code[0] == 0x8b && code[1] == 0x41 && code[3] == 0xc3) {
        if (code[2] & 0x80) { return {}; }
        offset = code[2];
    } else if (code.size() >= 7 && code[0] == 0x8b && code[1] == 0x81 && code[6] == 0xc3) {
        std::memcpy(&offset, code.data() + 2, sizeof(offset));
    } else { return {}; }
    return offset >= 8 && offset <= 0x100 && (offset & 3) == 0 ? std::optional{offset} : std::nullopt;
}

enum class Status { off, waiting, active, already_off, unavailable, external_change,
    restore_pending, unsupported, hook_failed, conflicting_options };

struct ConsoleValue {
    uintptr_t object{};
    uint32_t flags{};
    int value{};
    bool valid() const { return object && !(flags & 0xcu) && (value == 0 || value == 1); }
};

// Do not steal priority, rewrite flags, or restore over another console owner.
class ConsoleLease {
public:
    bool pending() const { return m_original.has_value(); }

    template<class Read, class Write> Status apply(Read&& read, Write&& write) {
        const auto current = read();
        if (!current || !current->valid()) { return Status::unavailable; }
        if (m_original) {
            if (!owned(*current)) { m_original.reset(); return Status::external_change; }
            return Status::active;
        }
        if (!current->value) { return Status::already_off; }
        m_original = current;
        write(*current, 0);
        const auto after = read();
        if (!after || !after->valid()) { return Status::restore_pending; }
        if (owned(*after)) { return Status::active; }
        if (after->object == current->object && after->flags == current->flags && after->value == current->value) {
            m_original.reset();
            return Status::unavailable;
        }
        m_original.reset();
        return Status::external_change;
    }

    template<class Read, class Write> Status restore(Read&& read, Write&& write) {
        if (!m_original) { return Status::off; }
        const auto current = read();
        if (!current || !current->valid()) { return Status::restore_pending; }
        if (!owned(*current)) { m_original.reset(); return Status::external_change; }
        const auto original = *m_original;
        write(*current, original.value);
        const auto after = read();
        if (!after || !after->valid()) { return Status::restore_pending; }
        if (after->object != original.object || after->flags != original.flags) {
            m_original.reset(); return Status::external_change;
        }
        if (after->value != original.value) { return Status::restore_pending; }
        m_original.reset();
        return Status::off;
    }

private:
    bool owned(const ConsoleValue& value) const {
        return m_original && value.object == m_original->object && value.flags == m_original->flags && value.value == 0;
    }
    std::optional<ConsoleValue> m_original{};
};

inline constexpr size_t temporary_camera_flag = 0xd8;
class TickSuppression {
public:
    explicit TickSuppression(uint8_t& flag, bool enabled) : m_flag(enabled && flag == 0 ? &flag : nullptr) {
        if (m_flag) { *m_flag = 1; }
    }
    TickSuppression(const TickSuppression&) = delete;
    TickSuppression& operator=(const TickSuppression&) = delete;
    ~TickSuppression() { if (m_flag && *m_flag == 1) { *m_flag = 0; } }
private:
    uint8_t* m_flag{};
};

inline uint64_t fingerprint(std::span<const uint8_t> bytes) {
    uint64_t hash = 14695981039346656037ull;
    for (const auto byte : bytes) { hash = (hash ^ byte) * 1099511628211ull; }
    return hash;
}

struct CodeWitness { uint32_t rva, size; uint64_t hash; };
// Exact current-image contracts, including all relative operands. A changed
// build, detour or helper must reject the private-byte fallback, not guess.
inline constexpr std::array<CodeWitness, 4> component_witnesses{{
    {0x2f5a5c, 854, 0x5d213115b07ec933ull},
    {0x2f5db2, 272, 0x4ee085cb9f78b351ull},
    {0x2f5ec2, 168, 0xb9acbda165a645ddull},
    {0x2f5f6a, 272, 0xfddcbe7ae9de64feull},
}};

template<class Read, class Size> bool component_contract(Read&& read, Size&& size) {
    std::array<uint8_t, 854> bytes{};
    for (const auto& witness : component_witnesses) {
        const auto code = std::span{bytes}.first(witness.size);
        if (size(witness.rva) != witness.size || !read(witness.rva, code) || fingerprint(code) != witness.hash) { return false; }
    }
    return true;
}

inline bool exclusive_component(bool cvar, bool component) { return component && !cvar; }
inline bool suppression_allowed(bool armed, uint32_t expected_thread, uint32_t current_thread, uintptr_t component) {
    return armed && expected_thread && expected_thread == current_thread &&
        component >= 0x10000 && component < 0x0000800000000000ull && (component & 7) == 0;
}
}
