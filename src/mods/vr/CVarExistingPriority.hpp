#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>

namespace uevr::cvar_existing_priority {

inline constexpr uint32_t priority_mask = 0xff000000;
inline constexpr uint32_t normal_priority = 0x08000000;
inline constexpr uint32_t protected_flags = 0x1 | 0x4 | 0x8 | 0x10;
inline constexpr std::array<uint8_t, 4> flags_getter{0x8b, 0x41, 0x18, 0xc3};

struct Memory {
    void* context{};
    bool (*read)(void*, uintptr_t, void*, size_t){};
    bool (*module_owned)(void*, uintptr_t, size_t, bool executable){};

    template<class T> bool load(uintptr_t address, T& value) const {
        return read && address >= 0x10000 &&
            address <= (std::numeric_limits<uintptr_t>::max)() - sizeof(T) &&
            read(context, address, &value, sizeof(T));
    }
};

struct Proof {
    uintptr_t object{};
    uintptr_t table{};
    uintptr_t getter{};
    uint32_t flags{};
    bool operator==(const Proof&) const = default;
};

inline std::optional<Proof> read_flags(const Memory& memory, uintptr_t object) {
    Proof proof{object};
    std::array<uint8_t, flags_getter.size()> code{};
    if ((object & (alignof(uintptr_t) - 1)) || !memory.module_owned ||
        object > (std::numeric_limits<uintptr_t>::max)() - 0x1c ||
        !memory.load(object, proof.table) ||
        (proof.table & (alignof(uintptr_t) - 1)) ||
        !memory.module_owned(memory.context, proof.table, 4 * sizeof(uintptr_t), false) ||
        !memory.load(proof.table + 3 * sizeof(uintptr_t), proof.getter) ||
        !memory.module_owned(memory.context, proof.getter, code.size(), true) ||
        !memory.load(proof.getter, code) || code != flags_getter ||
        !memory.load(object + 0x18, proof.flags)) { return std::nullopt; }
    uintptr_t after{};
    return memory.load(object, after) && after == proof.table ? std::optional{proof} : std::nullopt;
}

struct Table {
    uintptr_t data{};
    uint32_t count{};
    uint32_t capacity{};
    bool operator==(const Table&) const = default;
};
struct Entry {
    uintptr_t key{};
    int32_t length{};
    int32_t capacity{};
    uintptr_t value{};
    int32_t hash[2]{};
};
static_assert(sizeof(Table) == 16 && sizeof(Entry) == 32);

// Only check the already-cached object's binding. Do not discover or execute
// console objects, and do not trust a stale raw pointer from the render thread.
inline bool registry_matches(const Memory& memory, uintptr_t manager,
    std::wstring_view name, uintptr_t object)
{
    if (name.empty() || name.size() > 255 || manager < 0x10000 || object < 0x10000 ||
        ((manager | object) & (alignof(uintptr_t) - 1)) ||
        manager > (std::numeric_limits<uintptr_t>::max)() - 8) {
        return false;
    }
    Table table{};
    if (!memory.load(manager + 8, table) || table.data < 0x10000 ||
        (table.data & (alignof(uintptr_t) - 1)) ||
        table.count == 0 || table.count > 65536 || table.capacity < table.count ||
        table.capacity > 131072 ||
        table.data > (std::numeric_limits<uintptr_t>::max)() - size_t{table.count} * sizeof(Entry)) {
        return false;
    }
    std::array<Entry, 128> block{};
    for (uint32_t begin = 0; begin < table.count; begin += uint32_t(block.size())) {
        const auto count = (std::min)(size_t{table.count - begin}, block.size());
        if (!memory.read || !memory.read(memory.context, table.data + size_t{begin} * sizeof(Entry),
                block.data(), count * sizeof(Entry))) { return false; }
        for (size_t i = 0; i < count; ++i) {
            const auto& entry = block[i];
            if (entry.value != object || entry.length != int32_t(name.size() + 1) ||
                entry.capacity < entry.length || entry.capacity > 1024) { continue; }
            std::array<uint16_t, 256> text{};
            const auto size = static_cast<size_t>(entry.length) * sizeof(uint16_t);
            if (entry.key < 0x10000 || entry.key > (std::numeric_limits<uintptr_t>::max)() - size ||
                !memory.read(memory.context, entry.key, text.data(), size) || text[name.size()] != L'\0') {
                return false;
            }
            bool equal = true;
            const auto lower = [](uint32_t ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; };
            for (size_t n = 0; n < name.size(); ++n) { equal = equal && lower(text[n]) == lower(name[n]); }
            if (!equal) { continue; }
            Table after{};
            Entry after_entry{};
            return memory.load(manager + 8, after) && table == after &&
                memory.load(table.data + size_t{begin + i} * sizeof(Entry), after_entry) &&
                std::memcmp(&entry, &after_entry, sizeof(Entry)) == 0;
        }
    }
    return false;
}

enum class State {
    idle, queued, dispatched, opt_out, unsupported, protected_variable,
    no_priority_conflict, stale_request, binding_changed, owner_changed,
    invalid_value, setter_unavailable
};
inline const char* name(State state) {
    switch (state) {
    case State::queued: return "queued";
    case State::dispatched: return "one-shot dispatched; see value readback";
    case State::opt_out: return "not explicitly requested";
    case State::unsupported: return "console interface/flags accessor not validated";
    case State::protected_variable: return "read-only, cheat or unregistered variable";
    case State::no_priority_conflict: return "no supported higher priority to preserve";
    case State::stale_request: return "cancelled or superseded by a newer edit";
    case State::binding_changed: return "console registry binding changed";
    case State::owner_changed: return "flags/owner changed before dispatch";
    case State::invalid_value: return "invalid or out-of-range requested value";
    case State::setter_unavailable: return "engine setter unavailable";
    default: return "not requested";
    }
}

struct Request {
    bool explicit_click{};
    bool supported_abi{};
    bool floating{};
    double value{};
    double minimum{};
    double maximum{};
};
struct Result {
    State state{State::idle};
    uint32_t priority{};
    std::optional<uint32_t> after_flags{};
};
struct Backend {
    std::function<bool()> current_request;
    std::function<uintptr_t()> resolve;
    std::function<std::optional<Proof>(uintptr_t)> flags;
    std::function<bool(uintptr_t)> registry_matches;
    std::function<bool(uintptr_t, double, uint32_t)> set;
};

inline Result apply_once(const Request& request, const Backend& backend) {
    if (!request.explicit_click) { return {State::opt_out}; }
    if (!request.supported_abi || !backend.current_request || !backend.resolve ||
        !backend.flags || !backend.registry_matches || !backend.set) { return {State::unsupported}; }
    if (!std::isfinite(request.value) || !std::isfinite(request.minimum) ||
        !std::isfinite(request.maximum) || request.minimum > request.maximum ||
        request.value < request.minimum || request.value > request.maximum ||
        (!request.floating && (std::trunc(request.value) != request.value ||
            request.value < (std::numeric_limits<int32_t>::min)() ||
            request.value > (std::numeric_limits<int32_t>::max)())) ||
        (request.floating && std::abs(request.value) > (std::numeric_limits<float>::max)())) {
        return {State::invalid_value};
    }
    if (!backend.current_request()) { return {State::stale_request}; }
    const auto object = backend.resolve();
    const auto proof = object ? backend.flags(object) : std::nullopt;
    if (!proof || proof->object != object) { return {State::unsupported}; }
    if (proof->flags & protected_flags) { return {State::protected_variable}; }
    const auto priority = proof->flags & priority_mask;
    if (priority <= normal_priority || priority > 0x0f000000) { return {State::no_priority_conflict}; }
    if (!backend.registry_matches(object)) { return {State::binding_changed}; }
    if (!backend.current_request()) { return {State::stale_request}; }
    if (backend.resolve() != object || !backend.registry_matches(object)) { return {State::binding_changed}; }
    const auto rechecked = backend.flags(object);
    if (!rechecked || *rechecked != *proof) { return {State::owner_changed}; }
    if (!backend.current_request()) { return {State::stale_request}; }
    if (!backend.set(object, request.value, priority)) { return {State::setter_unavailable, priority}; }
    const auto after = backend.flags(object);
    return {State::dispatched, priority, after ? std::optional{after->flags} : std::nullopt};
}

} // namespace uevr::cvar_existing_priority
