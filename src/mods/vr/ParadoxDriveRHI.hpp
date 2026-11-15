#pragma once

#include <limits>
#include <string_view>

#include <sdk/BoundedDiscovery.hpp>
#include "ParadoxDriveRHIContracts.hpp"
#include "UE584DevelopmentContracts.hpp"

namespace uevr::paradoxdrive_rhi {

enum class Input : uint8_t { LegacyRHI, VerifiedRDG, UnverifiedRDG };
inline constexpr size_t command_list_offset = 0xc0;
inline constexpr size_t builder_function_size = 0x63f;
inline constexpr const char* builder_signature =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 50 0F B7 AC 24 A0 00 00 00 48 8B F1";
inline constexpr size_t development_builder_function_size = 0x881;
inline constexpr const char* development_builder_signature =
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 50 0F B7 B4 24 B0 00 00 00 48 8B F9";

inline bool target(std::wstring_view path, uint32_t file_version_ms) {
    if (file_version_ms != ((5u << 16) | 8u)) { return false; }
    const auto separator = path.find_last_of(L"/\\");
    path = path.substr(separator == path.npos ? 0 : separator + 1);
    constexpr std::wstring_view expected = L"paradoxdrive-win64-shipping.exe";
    if (path.size() != expected.size()) { return false; }
    for (size_t i = 0; i < path.size(); ++i) {
        const auto c = path[i] >= L'A' && path[i] <= L'Z' ? path[i] + (L'a' - L'A') : path[i];
        if (c != expected[i]) { return false; }
    }
    return true;
}

inline bool contains(uintptr_t base, size_t size, uintptr_t address, size_t bytes) {
    return base >= 0x10000 && size != 0 && base <= (std::numeric_limits<uintptr_t>::max)() - size &&
        address >= base && address - base < size && bytes != 0 && bytes <= size - (address - base);
}

template<size_t N>
bool matches(const sdk::discovery::Memory& memory, uintptr_t address,
    const std::array<uint8_t, N>& code, const std::array<uint8_t, N>& mask) {
    std::array<uint8_t, N> actual{};
    if (!memory.read || !memory.executable || address > (std::numeric_limits<uintptr_t>::max)() - N ||
        !memory.executable(memory.context, address, N) || !memory.read(memory.context, address, actual.data(), N)) { return false; }
    for (size_t i = 0; i < N; ++i) {
        if ((actual[i] & mask[i]) != (code[i] & mask[i])) { return false; }
    }
    return true;
}

template<class FunctionSize>
bool render_texture_layout(const sdk::discovery::Memory& memory, uintptr_t base, size_t size,
    uintptr_t render_texture, FunctionSize&& function_size) {
    return contains(base, size, render_texture, contract::render_texture_code.size()) &&
        function_size(render_texture) == contract::render_texture_code.size() &&
        matches(memory, render_texture, contract::render_texture_code, contract::render_texture_mask);
}

template<class FunctionSize>
bool builder_layout(const sdk::discovery::Memory& memory, uintptr_t base, size_t size,
    uintptr_t builder, FunctionSize&& function_size) {
    return contains(base, size, builder, builder_function_size) && function_size(builder) == builder_function_size &&
        matches(memory, builder, contract::builder_prefix_code, contract::builder_prefix_mask);
}

template<class Scan, class FunctionSize>
bool discover(const sdk::discovery::Memory& memory, uintptr_t base, size_t size,
    uintptr_t render_texture, Scan&& scan, FunctionSize&& function_size) {
    if (!render_texture_layout(memory, base, size, render_texture, function_size)) { return false; }
    uintptr_t cursor = base;
    bool found{};
    for (size_t candidates = 0; candidates < 4 && cursor - base < size; ++candidates) {
        const auto candidate = scan(cursor, size - (cursor - base), builder_signature);
        if (!candidate) { return found; }
        if (*candidate < cursor || !contains(base, size, *candidate, 1)) { return false; }
        cursor = *candidate + 1;
        if (!builder_layout(memory, base, size, *candidate, function_size)) { continue; }
        if (found) { return false; }
        found = true;
    }
    return false;
}

template<class Scan, class FunctionSize>
bool discover_development(const sdk::discovery::Memory& memory, uintptr_t base, size_t size,
    uintptr_t render_texture, Scan&& scan, FunctionSize&& function_size) {
    namespace d = ue584_development::contract;
    if (!contains(base, size, render_texture, d::render_texture_code.size()) ||
        function_size(render_texture) != d::render_texture_code.size() ||
        !matches(memory, render_texture, d::render_texture_code, d::render_texture_mask)) { return false; }
    uintptr_t cursor = base;
    bool found{};
    for (size_t candidates = 0; candidates < 4 && cursor - base < size; ++candidates) {
        const auto candidate = scan(cursor, size - (cursor - base), development_builder_signature);
        if (!candidate) { return found; }
        if (*candidate < cursor || !contains(base, size, *candidate, 1)) { return false; }
        cursor = *candidate + 1;
        if (!contains(base, size, *candidate, development_builder_function_size) ||
            function_size(*candidate) != development_builder_function_size ||
            !matches(memory, *candidate, d::builder_prefix_code, d::builder_prefix_mask)) { continue; }
        if (found) { return false; }
        found = true;
    }
    return false;
}

template<class Scan, class FunctionSize>
Input classify_input(const sdk::discovery::Memory& memory, std::wstring_view path, uint32_t file_version_ms,
    uintptr_t base, size_t size, uintptr_t render_texture, Scan&& scan, FunctionSize&& function_size) {
    const bool guarded_title = target(path, file_version_ms);
    if (!guarded_title && file_version_ms != ((5u << 16) | 8u)) { return Input::LegacyRHI; }
    // The stock UE5.8.4 Shipping callback independently matches this exact RDG
    // contract. Never probe the constructor or reinterpret arguments on a guess.
    if (!render_texture_layout(memory, base, size, render_texture, function_size)) {
        namespace d = ue584_development::contract;
        const bool development_callback = contains(base, size, render_texture, d::render_texture_code.size()) &&
            function_size(render_texture) == d::render_texture_code.size() &&
            matches(memory, render_texture, d::render_texture_code, d::render_texture_mask);
        if (!development_callback) { return guarded_title ? Input::UnverifiedRDG : Input::LegacyRHI; }
        return discover_development(memory, base, size, render_texture, scan, function_size)
            ? Input::VerifiedRDG : Input::UnverifiedRDG;
    }
    return discover(memory, base, size, render_texture, scan, function_size)
        ? Input::VerifiedRDG : Input::UnverifiedRDG;
}

inline uintptr_t command_list(const sdk::discovery::Memory& memory, Input input, uintptr_t argument) {
    if (input == Input::LegacyRHI) { return argument; }
    if (input != Input::VerifiedRDG || argument < 0x10000 || argument % alignof(uintptr_t) != 0) { return 0; }
    const auto field = sdk::discovery::relative_address(argument, command_list_offset);
    uintptr_t immediate{}, readable_prefix{};
    if (!field || !memory.load(*field, immediate) || immediate < 0x10000 || immediate == argument ||
        immediate % alignof(uintptr_t) != 0 || !memory.load(immediate, readable_prefix)) { return 0; }
    // The worker still validates the live Root and its module-owned command vtable.
    return immediate;
}

} // namespace uevr::paradoxdrive_rhi
