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

// Re-acquire only the live-traced game-code reset. Never raise the current
// priority, rewrite flags, or restore over another console owner.
class ConsoleLease {
public:
    bool pending() const { return m_original.has_value(); }
    bool reset_attempted() const { return m_reset_attempted; }

    template<class Read, class Write> Status apply(Read&& read, Write&& write, bool allow_game_reset = false) {
        m_reset_attempted = false;
        const auto current = read();
        if (!current || !current->valid()) { return Status::unavailable; }
        if (m_original) {
            if (owned(*current)) { return Status::active; }
            if (!allow_game_reset || !game_code_reset(*current)) {
                m_original.reset(); return Status::external_change;
            }
            m_reset_attempted = true;
            m_original.reset();
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
    bool game_code_reset(const ConsoleValue& current) const {
        constexpr uint32_t priority_mask = 0xff000000u, set_by_code = 0x0e000000u;
        const auto previous_priority = m_original->flags & priority_mask;
        return current.object == m_original->object && current.value == 1 &&
            (current.flags & priority_mask) == set_by_code &&
            (previous_priority == 0 || previous_priority == set_by_code) &&
            (current.flags & ~priority_mask) == (m_original->flags & ~priority_mask);
    }
    bool owned(const ConsoleValue& value) const {
        return m_original && value.object == m_original->object && value.flags == m_original->flags && value.value == 0;
    }
    std::optional<ConsoleValue> m_original{};
    bool m_reset_attempted{};
};

// Health polls are two seconds apart. Repeated resets exhaust this allowance;
// only ten seconds of successful ownership replenishes it, not each write.
class GameResetBudget {
public:
    static constexpr uint32_t limit = 3, stable_polls = 5;
    bool allowed() const { return m_attempts < limit; }
    uint32_t attempts() const { return m_attempts; }
    void observe(bool reset_attempted, Status status) {
        if (reset_attempted) {
            if (m_attempts < limit) { ++m_attempts; }
            m_stable_polls = 0;
        } else if (status == Status::active && m_attempts) {
            if (++m_stable_polls >= stable_polls) { m_attempts = m_stable_polls = 0; }
        } else { m_stable_polls = 0; }
    }
private:
    uint32_t m_attempts{}, m_stable_polls{};
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
// Exact image contracts, including all relative operands. Never mix helpers
// from different builds or use a matching Tick with unproven companion code.
inline constexpr std::array<CodeWitness, 4> component_witnesses{{
    {0x2f5a5c, 854, 0x5d213115b07ec933ull},
    {0x2f5db2, 272, 0x4ee085cb9f78b351ull},
    {0x2f5ec2, 168, 0xb9acbda165a645ddull},
    {0x2f5f6a, 272, 0xfddcbe7ae9de64feull},
}};

inline constexpr std::array<CodeWitness, 4> component_witnesses_386500{{
    {0x2f1fc4, 854, 0xe1668e51551427d0ull},
    {0x2f231a, 272, 0xad4d945dc50b2c5dull},
    {0x2f242a, 168, 0x722a63f1a33b294cull},
    {0x2f24d2, 272, 0xc368d6423e0f9476ull},
}};
inline constexpr std::array component_variants{component_witnesses, component_witnesses_386500};

template<class Read, class Size> std::optional<uint32_t> component_tick_rva(Read&& read, Size&& size) {
    std::array<uint8_t, 854> bytes{};
    std::optional<uint32_t> selected{};
    for (const auto& variant : component_variants) {
        bool valid = true;
        for (const auto& witness : variant) {
            if (witness.size > bytes.size() || size(witness.rva) != witness.size) { valid = false; break; }
            const auto code = std::span{bytes}.first(witness.size);
            if (!read(witness.rva, code) || fingerprint(code) != witness.hash) {
                valid = false; break;
            }
        }
        if (valid) {
            if (selected) { return {}; }
            selected = variant.front().rva;
        }
    }
    return selected;
}

template<class Read, class Size> bool component_contract(Read&& read, Size&& size) {
    return component_tick_rva(read, size).has_value();
}

inline bool exclusive_component(bool cvar, bool component) { return component && !cvar; }
inline bool suppression_allowed(bool armed, uint32_t expected_thread, uint32_t current_thread, uintptr_t component) {
    return armed && expected_thread && expected_thread == current_thread &&
        component >= 0x10000 && component < 0x0000800000000000ull && (component & 7) == 0;
}
}
