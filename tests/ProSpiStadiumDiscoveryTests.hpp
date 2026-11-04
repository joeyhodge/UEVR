#pragma once

#include "mods/vr/ProSpiStadiumDiscovery.hpp"
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace prospi_stadium_discovery_tests {
using namespace uevr::prospi::roof;

inline void check(bool value, const char* message) {
    if (!value) { throw std::runtime_error(message); }
}

inline std::vector<uint8_t> bytes(std::string_view text) {
    check(text.size() % 2 == 0, "complete discovery fixture bytes");
    std::vector<uint8_t> result;
    for (size_t i{}; i < text.size(); i += 2) {
        result.push_back(static_cast<uint8_t>(std::stoul(std::string{text.substr(i, 2)}, nullptr, 16)));
    }
    return result;
}

inline void contracts(const nlohmann::json& fixture) {
    for (const auto& row : fixture.at("cases")) {
        std::map<uintptr_t, DiscoveryCode> entries;
        for (const auto& item : row.at("entries")) {
            DiscoveryCode code;
            const auto source = bytes(item.at("code").get<std::string>());
            check(source.size() <= code.bytes.size(), "bounded captured code");
            std::copy(source.begin(), source.end(), code.bytes.begin());
            code.size = source.size(); code.original_entry = item.at("original_entry").get<bool>();
            entries.emplace(item.at("address").get<uintptr_t>(), code);
        }
        const auto captured = entries;
        const auto root = row.at("exec").get<uintptr_t>();
        const auto size = row.at("actor_size").get<uint32_t>();
        std::map<uintptr_t, DiscoveryFunctionPart> parts;
        const auto record = [](const nlohmann::json& value) {
            return DiscoveryUnwindRecord{value.at(0).get<uint32_t>(), value.at(1).get<uint32_t>(), value.at(2).get<uint32_t>()};
        };
        for (const auto& item : row.at("unwind_parts")) {
            DiscoveryFunctionPart part{item.at("begin").get<uintptr_t>(), item.at("end").get<uintptr_t>(), record(item.at("record"))};
            if (!item.at("chained").is_null()) { part.chained = record(item.at("chained")); }
            parts.emplace(part.begin, part);
        }
        bool omit_chained{};
        const auto lookup = [&](uintptr_t address) -> std::optional<DiscoveryFunctionPart> {
            const auto it = parts.find(address);
            return it != parts.end() && !(omit_chained && it->second.chained) ? std::optional{it->second} : std::nullopt;
        };
        size_t inspections{};
        const auto inspect = [&](uintptr_t address) -> std::optional<DiscoveryCode> {
            ++inspections;
            const auto it = entries.find(address);
            if (it == entries.end()) { return {}; }
            auto code = it->second;
            if (code.original_entry && code.size <= code.bytes.size()) {
                const auto extent = discovery_extent(address, lookup);
                if (!extent) { return {}; }
                code.size = (std::min)(code.size, *extent);
            }
            return code;
        };
        const auto discover = [&] { return discover_owned_components(root, size, inspect); };
        check(discover() == row.at("owned_components_offset").get<uint32_t>(), "actual edition ownership chain accepted");
        check(inspections <= 5, "code discovery does not sweep the image");
        omit_chained = true;
        check(!discover(), "real owner stride lies past the first unwind region; truncating there must fail closed");
        omit_chained = false;
        check(!discover_owned_components(root, 0x1ef, inspect), "owned set must fit reflected actor size");
        check(!discover_owned_components(root, 0x10001, inspect), "implausible class size rejected");
        inspections = 0;
        check(!discover_owned_components(0, size, inspect) && inspections == 0, "null function is never read");
        for (const auto& item : row.at("entries")) {
            const auto address = item.at("address").get<uintptr_t>();
            const auto role = item.at("role").get<std::string>();
            entries = captured;
            entries.erase(address);
            check(!discover(), "every contract stage must be readable and image-owned");
            entries = captured;
            auto& code = entries.at(address);
            if (!code.original_entry) {
                code.original_entry = true;
                check(!discover(), "an entry detour is not treated as an original leaf thunk");
                continue;
            }
            code.original_entry = false;
            check(!discover(), "non-leaf code needs exact function-entry validation");
            entries = captured;
            const auto prefix = role == "exec" ? 37u : role == "query" ? 32u : role == "owner" ? 30u : 32u;
            for (size_t i{}; i < prefix; ++i) {
                if (role == "query" && i >= 19 && i < 23) { continue; }
                entries = captured;
                entries.at(address).bytes[i] ^= 0x80;
                check(!discover(), "register forwarding and native container prefix are mandatory");
            }
            size_t required = role == "exec" ? 42u : role == "query" ? 32u : 0x7au;
            if (role == "owner") {
                constexpr std::array<uint8_t, 10> stride{0x48,0x8b,0x00,0x48,0x03,0xc9,0x48,0x8b,0x34,0xc8};
                const auto& original = captured.at(address);
                const auto found = std::search(original.bytes.begin(), original.bytes.begin() + original.size,
                    stride.begin(), stride.end());
                check(found != original.bytes.begin() + original.size, "captured native sparse-element stride exists");
                required = size_t(found - original.bytes.begin()) + stride.size();
                entries = captured;
                entries.at(address).bytes[required - 1] ^= 0x80;
                check(!discover(), "a different element stride cannot authorize this layout");
                for (const auto offset : {uint32_t{0}, uint32_t{0x1a1}, uint32_t{0x200}, UINT32_MAX}) {
                    entries = captured;
                    std::memcpy(entries.at(address).bytes.data() + 30, &offset, 4);
                    check(!discover(), "decoded owner offset must be aligned and fit the actor");
                }
            }
            for (size_t length{}; length < required; ++length) {
                entries = captured;
                entries.at(address).size = length;
                check(!discover(), "truncated code cannot establish ownership");
            }
            entries = captured;
            entries.at(address).size = entries.at(address).bytes.size() + 1;
            check(!discover(), "an oversized reader result cannot escape the local code buffer");
        }
    }
    const auto call = bytes("e8fbffffff");
    check(discovery_call(0x20000, call) == 0x20000, "negative rel32 decoded without unsigned underflow");
    for (size_t length{}; length < call.size(); ++length) {
        check(!discovery_call(0x20000, std::span{call}.first(length)), "short call is not read past its span");
    }
    check(!discovery_call(UINTPTR_MAX - 4, call), "next-instruction overflow rejected");
    check(!discovery_call(0, bytes("e880ffffff")), "negative call target underflow rejected");
    check(!discovery_call(UINTPTR_MAX - 5, bytes("e801000000")), "positive target overflow rejected");
    check(!discovery_call(0x20000, bytes("e900000000")), "only direct calls are decoded as calls");
    DiscoveryCode jump;
    jump.size = 5; jump.bytes = {0xe9, 0xfb, 0xff, 0xff, 0xff};
    size_t reads{};
    uintptr_t address = 0x20000;
    check(!discovery_body(address, [&](uintptr_t) -> std::optional<DiscoveryCode> { ++reads; return jump; }) && reads == 1,
        "leaf cycle is rejected before a second read");
}

inline void unwind_regions() {
    constexpr uintptr_t image = 0x140000000, entry = image + 0x27ff780;
    const DiscoveryUnwindRecord root{0x27ff780, 0x27ff8fa, 0xb8a6158};
    const DiscoveryFunctionPart first{entry, image + root.end, root};
    const DiscoveryFunctionPart second{image + 0x27ff8fa, image + 0x27ff97e,
        {0x27ff8fa, 0x27ff97e, 0xb8a6170}, root};
    std::map<uintptr_t, DiscoveryFunctionPart> entries{{first.begin, first}, {second.begin, second}};
    size_t reads{};
    const auto lookup = [&](uintptr_t address) -> std::optional<DiscoveryFunctionPart> {
        ++reads;
        const auto it = entries.find(address);
        return it != entries.end() ? std::optional{it->second} : std::nullopt;
    };
    check(discovery_extent(entry, lookup) == 510, "actual chained 2025 owner region is included");
    entries.at(second.begin).chained.reset();
    check(discovery_extent(entry, lookup) == 378, "an adjacent independent function is never included");
    entries.at(second.begin) = second; entries.at(second.begin).chained->unwind ^= 4;
    check(discovery_extent(entry, lookup) == 378, "chain must identify the entire original unwind record");
    entries.at(second.begin) = second; entries.at(second.begin).begin += 1;
    check(discovery_extent(entry, lookup) == 378, "gap or inconsistent continuation metadata is rejected");
    entries.at(second.begin) = second; entries.at(second.begin).end += uintptr_t{1} << 32;
    check(discovery_extent(entry, lookup) == 378, "foreign-image continuation cannot match a main-image RVA");
    entries.at(first.begin).end = first.begin;
    check(!discovery_extent(entry, lookup), "empty unwind range is rejected");
    reads = 0;
    check(!discovery_extent(0, lookup) && reads == 0, "null metadata lookup is not attempted");
}

inline void containers() {
    check(valid_actor_array(0, 0, 0), "empty actor list is permitted but supplies no candidate");
    check(valid_actor_array(0x10000, 32768, 65536), "bounded native actor list accepted");
    check(!valid_actor_array(0, 1, 1) && !valid_actor_array(0x10001, 1, 1), "null and misaligned actor arrays rejected");
    check(!valid_actor_array(UINTPTR_MAX - 7, 2, 2), "actor-array span overflow rejected");
    check(!valid_actor_array(0x10000, -1, 1) && !valid_actor_array(0x10000, 2, 1) &&
        !valid_actor_array(0x10000, 32769, 65536), "invalid actor count and capacity rejected");
    OwnedComponentSet set;
    set.first_free = -1;
    check(set.valid() && !set.element(0), "empty native set is valid and cannot yield elements");
    set.elements = 0x20000; set.slots = set.bits = 100; set.capacity = set.bit_capacity = 128;
    set.inline_bits[0] = 1; set.inline_bits[3] = 8;
    check(set.valid() && set.allocated_inline(0) && set.allocated_inline(99), "inline allocation endpoints accepted");
    check(!set.allocated_inline(1) && !set.allocated_inline(100) && !set.element(100), "holes and end iterator never read");
    check(set.element(99) == 0x20630 && !set.bit_word(0), "sparse entries use native 16-byte stride");
    set.slots = set.bits = 409; set.capacity = 585; set.bit_capacity = 768; set.external_bits = 0x40000;
    check(set.valid() && set.bit_word(408) == 0x40030 && !set.allocated_inline(0), "captured heap bitmap layout accepted");
    const auto good = set;
    for (const auto member : {&OwnedComponentSet::slots, &OwnedComponentSet::capacity, &OwnedComponentSet::bits,
            &OwnedComponentSet::bit_capacity, &OwnedComponentSet::first_free, &OwnedComponentSet::free_count}) {
        auto changed = good; ++(changed.*member);
        check(!good.same_storage(changed), "container mutation invalidates cursor storage");
    }
    auto changed = good; changed.elements += 16;
    check(!good.same_storage(changed), "element allocation changes invalidate cursor storage");
    changed = good; changed.external_bits += 4;
    check(!good.same_storage(changed), "bitmap allocation changes invalidate cursor storage");
    changed = good; changed.inline_bits[0] ^= 1;
    check(!good.same_storage(changed), "inline allocation-bit changes invalidate cursor storage");
    for (const auto bad_pointer : {uintptr_t{0}, uintptr_t{0x10001}, UINTPTR_MAX - 7}) {
        changed = good; changed.elements = bad_pointer;
        check(!changed.valid(), "invalid sparse allocation rejected");
    }
    changed = good; changed.external_bits = 0;
    check(!changed.valid(), "large allocation cannot claim inline bitmap storage");
    changed = good; changed.free_count = 1;
    check(!changed.valid(), "free count requires a valid free-list head");
    changed.first_free = 400;
    check(changed.valid(), "native sparse holes remain supported");
    changed = good; changed.capacity = max_owned_components + 1;
    check(!changed.valid(), "unbounded allocation is rejected");

    constexpr uintptr_t descriptor = 0x50000, component = 0x60000;
    size_t reads{}, bit_reads{};
    bool mutate_header{}, mutate_bitmap{}, fail_read{};
    const auto read = [&](uintptr_t where, auto& output) {
        ++reads;
        if (fail_read) { return false; }
        using T = std::remove_cvref_t<decltype(output)>;
        if constexpr (std::is_same_v<T, OwnedComponentSet>) {
            if (where != descriptor) { return false; }
            output = good;
            if (mutate_header) { output.elements += 16; }
        } else if constexpr (std::is_same_v<T, uint32_t>) {
            if (where != *good.bit_word(408)) { return false; }
            output = uint32_t{1} << (408 % 32);
            if (mutate_bitmap && ++bit_reads > 1) { output = 0; }
        } else {
            if (where != *good.element(408)) { return false; }
            output = component;
        }
        return true;
    };
    check(read_owned_component(good, descriptor, 408, read) == component && reads == 4,
        "actual helper rechecks descriptor and allocation bit before publishing a candidate");
    mutate_header = true;
    check(!read_owned_component(good, descriptor, 408, read), "reallocation between reads rejects candidate");
    mutate_header = false; mutate_bitmap = true; bit_reads = 0;
    check(!read_owned_component(good, descriptor, 408, read), "concurrent free invalidates candidate");
    mutate_bitmap = false; fail_read = true;
    check(!read_owned_component(good, descriptor, 408, read), "unreadable storage falls back without game calls");
    fail_read = false; reads = 0;
    check(!read_owned_component(good, descriptor, 409, read) && reads == 0, "past-end sparse slot is never read");
    check(!read_owned_component(good, UINTPTR_MAX - 7, 408, read) && reads == 0, "descriptor span overflow rejected");
}

inline void run(const std::filesystem::path& path) {
    std::ifstream input{path};
    check(input.good(), "native ownership fixture must be present");
    contracts(nlohmann::json::parse(input));
    unwind_regions();
    containers();
}
} // namespace prospi_stadium_discovery_tests
