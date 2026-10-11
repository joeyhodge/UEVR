#pragma once

#include "DuneFrameHandoff.hpp"
#include "DuneRendererAbi.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>

namespace uevr::dune_frame {

// Linked RHI command -> FRDGBuilder constructor -> view-extension dispatcher.
// Only RIP-relative addresses vary. Fields, argument registers and stack slots
// remain mandatory; a changed layout cannot inherit the old callback ABI.
inline constexpr std::array<uint8_t, 77> bridge_code{
    0x48,0x8B,0x47,0x08,0x48,0x89,0x08,0x48,0x8D,0x41,0x08,0x48,0x89,0x47,0x08,
    0x4C,0x89,0x28,0x4C,0x89,0x21,0x4C,0x89,0x71,0x10,0x48,0x8D,0x15,0,0,0,0,
    0x48,0x8D,0x4D,0,0xE8,0,0,0,0,0x4C,0x8B,0xC0,0x48,0x8D,0x8D,0x80,0,0,0,
    0x45,0x33,0xC9,0x48,0x8B,0xD7,0xE8,0,0,0,0,
    0x48,0x8B,0xD6,0x48,0x8D,0x8D,0x80,0,0,0,0xE8,0,0,0,0};
inline constexpr std::array<size_t, 4> bridge_displacements{28, 37, 58, 73};
inline constexpr std::array<uint8_t, 70> graph_constructor_code{
    0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x6C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,
    0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x20,0x41,0x8B,0xE9,0x49,0x8B,0xF0,
    0x48,0x8B,0xFA,0x48,0x8B,0xD9,0xE8,0,0,0,0,0x48,0x89,0x03,0x45,0x33,0xFF,
    0x4C,0x89,0x7B,0x10,0x4C,0x89,0x7B,0x20,0x44,0x89,0x7B,0x28,0x48,0x8B,0x03,
    0x48,0x89,0x7B,0x50,0x48,0x89,0x43,0x58};
inline constexpr std::array<size_t, 1> graph_displacements{37};
inline constexpr std::array<uint8_t, 21> dispatcher_prefix{
    0x40,0x53,0x41,0x56,0x48,0x83,0xEC,0x48,0x83,0xBA,0xD8,0,0,0,0,
    0x48,0x8B,0xDA,0x4C,0x8B,0xF1};
inline constexpr size_t dispatcher_scan_size = 0x200;

// GDK cannot inherit a Win64 source mapping. Its own plural renderer must
// also prove the game-thread family argument at BeginRenderViewFamily slot 5.
inline constexpr auto begin_family_code = std::to_array<uint8_t>({
    0x49,0x8B,0x1F,0x45,0x33,0xF6,0x44,0x39,0xB3,0xB8,0,0,0,0x7E,0x26,0x33,0xF6,
    0x48,0x8B,0x83,0xB0,0,0,0,0x48,0x8B,0xD3,0x48,0x8B,0x0C,0x06,0x48,0x8B,0x01,
    0xFF,0x50,0x28,0x41,0xFF,0xC6,0x48,0x8D,0x76,0x10,0x44,0x3B,0xB3,0xB8,0,0,0,0x7C,0xDC});

inline bool valid_begin_family_callback(std::span<const uint8_t> code) {
    if (code.size() > dune_renderer::maximum_abi_bytes) { return false; }
    const auto found = std::search(code.begin(), code.end(), begin_family_code.begin(), begin_family_code.end());
    return found != code.end() &&
        std::search(found + 1, code.end(), begin_family_code.begin(), begin_family_code.end()) == code.end();
}

template <size_t N>
constexpr bool displacement_byte(size_t index, const std::array<size_t, N>& displacements) {
    for (auto start : displacements) {
        if (index >= start && index - start < sizeof(int32_t)) { return true; }
    }
    return false;
}

template <size_t N, size_t M>
bool matches_masked(std::span<const uint8_t> code, const std::array<uint8_t, N>& expected,
    const std::array<size_t, M>& displacements) {
    if (code.size() < expected.size()) { return false; }
    for (size_t i = 0; i < expected.size(); ++i) {
        if (!displacement_byte(i, displacements) && code[i] != expected[i]) { return false; }
    }
    return true;
}

inline std::string bridge_pattern() {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(bridge_code.size() * 3);
    for (size_t i = 0; i < bridge_code.size(); ++i) {
        if (i) { result += ' '; }
        if (displacement_byte(i, bridge_displacements)) { result += '?'; }
        else { result += hex[bridge_code[i] >> 4]; result += hex[bridge_code[i] & 15]; }
    }
    return result;
}

struct CodeImage {
    uintptr_t base{};
    size_t size{};

    bool contains(uintptr_t address, size_t length) const {
        return base && size && base <= std::numeric_limits<uintptr_t>::max() - size &&
            address >= base && address - base < size && length && length <= size - (address - base);
    }
};

inline std::optional<uintptr_t> relative_call(CodeImage image, uintptr_t address,
    std::span<const uint8_t> code) {
    if (code.size() < 5 || code[0] != 0xE8 || !image.contains(address, 5)) { return {}; }
    int32_t displacement{};
    std::memcpy(&displacement, code.data() + 1, sizeof(displacement));
    const auto next = address + 5;
    const auto distance = static_cast<uint64_t>(displacement < 0 ? -int64_t{displacement} : displacement);
    if (displacement < 0 ? next < distance : distance > std::numeric_limits<uintptr_t>::max() - next) { return {}; }
    const auto target = displacement < 0 ? next - distance : next + distance;
    return image.contains(target, 1) ? std::optional<uintptr_t>{target} : std::nullopt;
}

struct DiscoveredContract {
    uintptr_t bridge{};
    uintptr_t graph_constructor{};
    uintptr_t dispatcher{};
    uintptr_t renderer_calls{};
};

// Entry validation checks module ownership, executable memory and unwind bounds.
// Only one bridge is acceptable. No hooks or game memory writes occur here.
template <typename Read, typename Entry, typename ContainingFunction, typename OwnsCode>
std::optional<DiscoveredContract> discover_contract(CodeImage image,
    uintptr_t renderer_entry, std::span<const uint8_t> renderer_code, std::span<const uintptr_t> bridges,
    Read&& read, Entry&& entry, ContainingFunction&& contains_function, OwnsCode&& owns_code) {
    if (!image.contains(renderer_entry, renderer_code.size()) || !dune_renderer::valid_r14_family_abi(renderer_code) ||
        !entry(renderer_entry, renderer_code.size()) ||
        bridges.size() != 1 || !image.contains(bridges[0], bridge_code.size()) ||
        !contains_function(bridges[0], bridge_code.size())) { return {}; }
    std::array<uint8_t, bridge_code.size()> bridge{};
    if (!read(bridges[0], bridge.data(), bridge.size()) ||
        !matches_masked(bridge, bridge_code, bridge_displacements)) { return {}; }
    const auto constructor = relative_call(image, bridges[0] + 57, std::span{bridge}.subspan(57));
    const auto dispatcher = relative_call(image, bridges[0] + 72, std::span{bridge}.subspan(72));
    if (!constructor || !dispatcher || *constructor == *dispatcher ||
        !image.contains(*constructor, graph_constructor_code.size()) ||
        !image.contains(*dispatcher, dispatcher_scan_size) ||
        !entry(*constructor, graph_constructor_code.size()) || !entry(*dispatcher, dispatcher_prefix.size())) { return {}; }

    std::array<uint8_t, graph_constructor_code.size()> graph{};
    if (!read(*constructor, graph.data(), graph.size()) ||
        !matches_masked(graph, graph_constructor_code, graph_displacements)) { return {}; }
    const auto allocator = relative_call(image, *constructor + 36, std::span{graph}.subspan(36));
    if (!allocator || *allocator == *constructor || *allocator == *dispatcher || !entry(*allocator, 16)) { return {}; }

    std::array<uint8_t, dispatcher_scan_size> callbacks{};
    if (!read(*dispatcher, callbacks.data(), callbacks.size()) ||
        !std::equal(dispatcher_prefix.begin(), dispatcher_prefix.end(), callbacks.begin())) { return {}; }
    const auto found = std::search(callbacks.begin() + dispatcher_prefix.size(), callbacks.end(),
        renderer_calls.begin(), renderer_calls.end());
    if (found == callbacks.end() || found - callbacks.begin() < 7 ||
        !std::equal(found - 7, found, std::array<uint8_t, 7>{0x48,0x8B,0x83,0xD0,0,0,0}.begin()) ||
        std::search(found + renderer_calls.size(), callbacks.end(), renderer_calls.begin(), renderer_calls.end()) != callbacks.end()) { return {}; }
    const auto calls = *dispatcher + static_cast<size_t>(found - callbacks.begin());
    if (!owns_code(*dispatcher, calls - 7, renderer_calls.size() + 7)) { return {}; }
    return DiscoveredContract{bridges[0], *constructor, *dispatcher, calls};
}

} // namespace uevr::dune_frame
