#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace uevr::prospi::roof {

inline constexpr size_t max_owned_components = 8192;
inline constexpr size_t level_actors_candidate_offset = 0x98;

inline bool valid_actor_array(uintptr_t data, int32_t count, int32_t capacity) noexcept {
    return count >= 0 && count <= 32768 && capacity >= count && capacity <= 65536 &&
        (!count || (data >= 0x10000 && data % alignof(uintptr_t) == 0 &&
            data <= UINTPTR_MAX - size_t(count) * sizeof(uintptr_t)));
}

struct DiscoveryCode {
    std::array<uint8_t, 1024> bytes{};
    size_t size{};
    bool original_entry{};
};

struct DiscoveryUnwindRecord {
    uint32_t begin{}, end{}, unwind{};
    bool operator==(const DiscoveryUnwindRecord&) const = default;
};

struct DiscoveryFunctionPart {
    uintptr_t begin{}, end{};
    DiscoveryUnwindRecord record;
    std::optional<DiscoveryUnwindRecord> chained;
};

template<class Lookup>
std::optional<size_t> discovery_extent(uintptr_t address, Lookup&& lookup) {
    auto part = address >= 0x10000 ? lookup(address) : std::nullopt;
    if (!part || part->begin != address || part->end <= address || part->end - address > 4096) { return {}; }
    if (part->record.begin >= part->record.end || part->begin < part->record.begin) { return {}; }
    const auto image = part->begin - part->record.begin;
    const auto in_image = [image](const DiscoveryFunctionPart& value) {
        return image <= UINTPTR_MAX - value.record.end && value.record.begin < value.record.end &&
            value.begin == image + value.record.begin && value.end == image + value.record.end;
    };
    if (image < 0x10000 || !in_image(*part)) { return {}; }
    const auto root = part->record;
    // Both editions put the sparse-element load in a second unwind region.
    // An adjacent function is not a continuation unless its chain names one
    // of the exact already-validated records in this function.
    std::array<DiscoveryUnwindRecord, 3> seen{};
    size_t count{};
    for (;;) {
        seen[count++] = part->record;
        const auto length = part->end - address;
        if (length >= 1024 || count == seen.size()) { return (std::min)(length, size_t{1024}); }
        auto next = lookup(part->end);
        if (!next || !in_image(*next) || next->begin != part->end || next->end <= next->begin ||
            next->end < address || next->end - address > 4096 || !next->chained ||
            (next->chained != root && std::find(seen.begin(), seen.begin() + count, *next->chained) == seen.begin() + count)) {
            return length;
        }
        part = next;
    }
}

inline std::optional<uintptr_t> discovery_call(uintptr_t instruction, std::span<const uint8_t> bytes) {
    if (bytes.size() < 5 || instruction > UINTPTR_MAX - 5 || bytes[0] != 0xe8) { return {}; }
    int32_t displacement{};
    std::memcpy(&displacement, bytes.data() + 1, 4);
    const auto next = instruction + 5;
    if (displacement < 0) {
        const auto magnitude = uint64_t(-int64_t(displacement));
        return magnitude <= next ? std::optional<uintptr_t>{next - magnitude} : std::nullopt;
    }
    return uintptr_t(displacement) <= UINTPTR_MAX - next ?
        std::optional<uintptr_t>{next + uintptr_t(displacement)} : std::nullopt;
}

template<class Inspect>
std::optional<DiscoveryCode> discovery_body(uintptr_t& address, Inspect&& inspect) {
    std::array<uintptr_t, 3> visited{};
    for (size_t hop{}; hop < visited.size(); ++hop) {
        if (address < 0x10000 || std::find(visited.begin(), visited.begin() + hop, address) != visited.begin() + hop) { return {}; }
        visited[hop] = address;
        auto code = inspect(address);
        if (!code || !code->size || code->size > code->bytes.size() || address > UINTPTR_MAX - code->size) { return {}; }
        if (code->original_entry) { return code; }
        // The 2025 collector is an original same-image leaf jump, as is its
        // already-supported visibility setter. Never follow a detoured body.
        if (code->size != 5 || code->bytes[0] != 0xe9) { return {}; }
        auto jump = code->bytes;
        jump[0] = 0xe8;
        const auto target = discovery_call(address, jump);
        if (!target) { return {}; }
        address = *target;
    }
    return {};
}

template<class Inspect>
std::optional<uint32_t> discover_owned_components(uintptr_t exec, uint32_t actor_size, Inspect&& inspect) {
    // GetStadiumComponents' reflected exec builds a native output TArray.
    // Decode its actual query/collector chain; do not call the SDK's void stub.
    constexpr std::array<uint8_t, 37> exec_prefix{
        0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x30,0x4c,0x8b,0x4a,0x20,0x33,0xc0,
        0x4d,0x85,0xc9,0x49,0x8b,0xf8,0x0f,0x95,0xc0,0x49,0x03,0xc1,0x48,0x89,0x42,0x20,
        0x48,0x8d,0x54,0x24,0x20};
    auto root = discovery_body(exec, inspect);
    if (!root || root->size < 42 || !std::equal(exec_prefix.begin(), exec_prefix.end(), root->bytes.begin())) { return {}; }
    auto query = discovery_call(exec + exec_prefix.size(), std::span{root->bytes}.subspan(exec_prefix.size(), 5));
    if (!query) { return {}; }
    auto query_code = discovery_body(*query, inspect);
    constexpr std::array<uint8_t, 32> query_prefix{
        0x40,0x53,0x48,0x83,0xec,0x20,0x33,0xc0,0x48,0x8b,0xda,0x48,0x89,0x02,0x48,0x89,
        0x42,0x08,0xe8,0,0,0,0,0x48,0x8b,0xc3,0x48,0x83,0xc4,0x20,0x5b,0xc3};
    if (!query_code || query_code->size < query_prefix.size()) { return {}; }
    for (size_t i{}; i < query_prefix.size(); ++i) {
        if ((i < 19 || i >= 23) && query_code->bytes[i] != query_prefix[i]) { return {}; }
    }
    auto collector = discovery_call(*query + 18, std::span{query_code->bytes}.subspan(18, 5));
    if (!collector) { return {}; }
    auto collect_code = discovery_body(*collector, inspect);
    if (!collect_code || collect_code->size < 0x7a) { return {}; }
    constexpr std::array<std::array<uint8_t, 32>, 2> collector_prefix{{
        {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x57,0x48,0x83,
         0xec,0x20,0x8b,0x42,0x0c,0x33,0xf6,0x89,0x72,0x08,0x48,0x8b,0xfa,0x48,0x8b,0xe9},
        {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x57,0x48,0x83,
         0xec,0x20,0x8b,0x42,0x0c,0x31,0xf6,0x89,0x72,0x08,0x48,0x89,0xd7,0x48,0x89,0xcd}}};
    constexpr std::array<std::array<uint8_t, 14>, 2> collector_arguments{{
        {0x4c,0x8b,0xc7,0x48,0x8b,0xd3,0x48,0x8b,0xcd,0x48,0x3b,0xf0,0x75,0x07},
        {0x49,0x89,0xf8,0x48,0x89,0xda,0x48,0x89,0xe9,0x48,0x39,0xc6,0x75,0x07}}};
    bool forwards_actor{};
    for (size_t shape{}; shape < collector_prefix.size(); ++shape) {
        forwards_actor |= std::equal(collector_prefix[shape].begin(), collector_prefix[shape].end(), collect_code->bytes.begin()) &&
            std::equal(collector_arguments[shape].begin(), collector_arguments[shape].end(), collect_code->bytes.begin() + 0x67);
    }
    if (!forwards_actor) { return {}; }
    auto target = discovery_call(*collector + 0x75, std::span{collect_code->bytes}.subspan(0x75, 5));
    auto leaf = target ? discovery_body(*target, inspect) : std::nullopt;
    constexpr std::array<uint8_t, 30> owner_prefix{
        0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x4c,0x89,0x6c,0x24,0x20,
        0x55,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x83,0xec,0x70,0x4c,0x8d,0xa9};
    constexpr std::array<uint8_t, 10> element_stride{0x48,0x8b,0x00,0x48,0x03,0xc9,0x48,0x8b,0x34,0xc8};
    if (!leaf || leaf->size < 0x40 || !std::equal(owner_prefix.begin(), owner_prefix.end(), leaf->bytes.begin()) ||
        std::search(leaf->bytes.begin(), leaf->bytes.begin() + leaf->size, element_stride.begin(), element_stride.end()) == leaf->bytes.begin() + leaf->size) { return {}; }
    uint32_t offset{};
    std::memcpy(&offset, leaf->bytes.data() + owner_prefix.size(), 4);
    if (offset < 0x28 || offset % 8 || actor_size < 0x50 || actor_size > 0x10000 || offset > actor_size - 0x50) { return {}; }
    return offset;
}

struct OwnedComponentSet {
    uintptr_t elements{};
    int32_t slots{}, capacity{};
    std::array<uint32_t, 4> inline_bits{};
    uintptr_t external_bits{};
    int32_t bits{}, bit_capacity{}, first_free{}, free_count{};

    bool valid() const noexcept {
        return slots >= 0 && slots <= capacity && capacity <= max_owned_components &&
            slots == bits && bits <= bit_capacity && bit_capacity <= max_owned_components &&
            free_count >= 0 && free_count <= slots &&
            (free_count ? first_free >= 0 && first_free < slots : first_free == -1) &&
            (!slots || (elements >= 0x10000 && elements % 8 == 0 &&
                elements <= UINTPTR_MAX - size_t(slots) * 16)) &&
            (external_bits ? external_bits >= 0x10000 && external_bits % 4 == 0 &&
                external_bits <= UINTPTR_MAX - size_t((bits + 31) / 32) * 4 : bit_capacity <= 128);
    }
    bool same_storage(const OwnedComponentSet& other) const noexcept {
        return elements == other.elements && slots == other.slots && capacity == other.capacity &&
            external_bits == other.external_bits && bits == other.bits && bit_capacity == other.bit_capacity &&
            first_free == other.first_free && free_count == other.free_count && inline_bits == other.inline_bits;
    }
    std::optional<uintptr_t> element(size_t index) const noexcept {
        return valid() && index < size_t(slots) ?
            std::optional<uintptr_t>{elements + index * 16} : std::nullopt;
    }
    std::optional<uintptr_t> bit_word(size_t index) const noexcept {
        return valid() && external_bits && index < size_t(bits) ?
            std::optional<uintptr_t>{external_bits + (index / 32) * sizeof(uint32_t)} : std::nullopt;
    }
    bool allocated_inline(size_t index) const noexcept {
        return valid() && !external_bits && index < size_t(bits) &&
            (inline_bits[index / 32] & (uint32_t{1} << (index % 32))) != 0;
    }
};
static_assert(sizeof(OwnedComponentSet) == 0x38);

template<class Read>
std::optional<uintptr_t> read_owned_component(const OwnedComponentSet& before,
    uintptr_t descriptor, size_t index, Read&& read) {
    if (descriptor < 0x10000 || descriptor > UINTPTR_MAX - sizeof(OwnedComponentSet)) { return {}; }
    const auto element = before.element(index);
    if (!element) { return {}; }
    uint32_t allocation{}, confirmed{};
    const auto word = before.bit_word(index);
    if (word) {
        if (!read(*word, allocation) || !(allocation & (uint32_t{1} << (index % 32)))) { return {}; }
    } else if (!before.allocated_inline(index)) { return {}; }
    uintptr_t component{};
    OwnedComponentSet after{};
    if (!read(*element, component) || !read(descriptor, after) || !before.same_storage(after) ||
        (word && (!read(*word, confirmed) || confirmed != allocation)) ||
        component < 0x10000 || component % alignof(uintptr_t)) { return {}; }
    return component;
}

} // namespace uevr::prospi::roof
