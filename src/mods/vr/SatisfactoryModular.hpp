#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <sdk/BoundedDiscovery.hpp>
#include <sdk/TArray.hpp>

namespace sdk { class FSceneView; }

namespace uevr::satisfactory {

constexpr wchar_t lower(wchar_t ch) {
    return ch >= L'A' && ch <= L'Z' ? ch + (L'a' - L'A') : ch;
}

inline bool equal_path(std::wstring_view left, std::wstring_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](auto a, auto b) {
        if (a == L'/') { a = L'\\'; }
        if (b == L'/') { b = L'\\'; }
        return lower(a) == lower(b);
    });
}

inline std::wstring_view basename(std::wstring_view path) {
    const auto separator = path.find_last_of(L"/\\");
    return path.substr(separator == path.npos ? 0 : separator + 1);
}

inline std::wstring_view executable_prefix(std::wstring_view path) {
    const auto name = basename(path);
    if (equal_path(name, L"FactoryGameEGS-Win64-Shipping.exe")) { return L"FactoryGameEGS"; }
    if (equal_path(name, L"FactoryGameSteam-Win64-Shipping.exe")) { return L"FactoryGameSteam"; }
    return {};
}

constexpr bool ue561(uint32_t version_ms, uint32_t version_ls) {
    return version_ms == 0x00050006 && (version_ls >> 16) == 1;
}

inline bool supported_runtime(std::wstring_view path, uint32_t version_ms, uint32_t version_ls) {
    return !executable_prefix(path).empty() && ue561(version_ms, version_ls);
}

inline bool owns_module(std::wstring_view executable, std::wstring_view module, std::wstring_view component) {
    const auto prefix = executable_prefix(executable);
    const auto name = basename(module);
    const auto exe_separator = executable.find_last_of(L"/\\");
    const auto module_separator = module.find_last_of(L"/\\");
    constexpr std::wstring_view suffix = L"-Win64-Shipping.dll";
    return !prefix.empty() && !component.empty() && exe_separator != executable.npos && module_separator != module.npos &&
        equal_path(executable.substr(0, exe_separator), module.substr(0, module_separator)) &&
        name.size() == prefix.size() + 1 + component.size() + suffix.size() &&
        equal_path(name.substr(0, prefix.size()), prefix) && name[prefix.size()] == L'-' &&
        equal_path(name.substr(prefix.size() + 1, component.size()), component) &&
        equal_path(name.substr(prefix.size() + 1 + component.size()), suffix);
}

constexpr bool use_modular_renderer(bool supported, bool dx12, bool native_fix) {
    // is_native_stereo_fix_enabled() already excludes AFR, Mono and DIBR.
    return supported && dx12 && native_fix;
}

constexpr bool owns_renderer_pair(uintptr_t expected, uintptr_t callee_module, uintptr_t caller_module) {
    return expected != 0 && callee_module == expected && caller_module == expected;
}

// Only the misrouted, source-confirmed numeric controls use the new interface.
// Deprecated aliases and user-added/game-specific CVars keep their old path.
inline bool recover_console_variable(std::wstring_view name) {
    constexpr std::array names{L"r.OneFrameThreadLag", L"r.AllowOcclusionQueries", L"r.VolumetricCloud",
        L"r.AmbientOcclusionLevels", L"r.DepthOfFieldQuality", L"r.MotionBlurQuality", L"r.SceneColorFringeQuality",
        L"r.DefaultFeature.AmbientOcclusion", L"r.TemporalAA.Upsampling", L"r.ScreenPercentage"};
    return std::any_of(names.begin(), names.end(), [&](auto known) { return equal_path(name, known); });
}

struct Image {
    uintptr_t base{};
    size_t size{};

    bool contains(uintptr_t address, size_t length) const {
        return base >= 0x10000 && size != 0 && size <= (std::numeric_limits<uintptr_t>::max)() - base &&
            address >= base && length != 0 && address - base < size && length <= size - (address - base);
    }
};

struct ConsoleRetry {
    static constexpr uint32_t max_attempts = 6;
    static constexpr int64_t interval_ms = 1000;
    uint32_t attempts{};
    int64_t next_ms{};

    bool begin(int64_t now_ms) {
        if (now_ms < 0 || now_ms < next_ms || attempts >= max_attempts ||
            now_ms > (std::numeric_limits<int64_t>::max)() - interval_ms) { return false; }
        ++attempts;
        next_ms = now_ms + interval_ms;
        return true;
    }
};

inline std::optional<size_t> console_array_bytes(uintptr_t elements, uint32_t count, uint32_t capacity) {
    constexpr size_t entry_bytes = 32;
    constexpr uint32_t max_entries = 65536;
    if (elements < 0x10000 || elements % alignof(uintptr_t) != 0 || count == 0 ||
        count > capacity || capacity > max_entries) { return std::nullopt; }
    const auto bytes = size_t{count} * entry_bytes;
    if (bytes > (std::numeric_limits<uintptr_t>::max)() - elements) { return std::nullopt; }
    return bytes;
}

template <size_t Count>
bool code_matches(const sdk::discovery::Memory& memory, const Image& image, uintptr_t address,
    const std::array<uint8_t, Count>& expected) {
    std::array<uint8_t, Count> bytes{};
    return image.contains(address, bytes.size()) && memory.executable &&
        memory.executable(memory.context, address, bytes.size()) && memory.load(address, bytes) && bytes == expected;
}

enum class ConsoleType { Integer, Floating, Boolean };

// Matching modular Engine PDB and UE5.6.1 source. These are only accepted
// together with the exported constructor/destructor and view-accessor proofs.
inline constexpr size_t family_size = 0x198;
inline constexpr size_t family_interfaces_offset = 0x160;
inline constexpr size_t family_additional_offset = 0xB0;
inline constexpr size_t view_pass_offset = 0xDD0;
inline constexpr size_t view_primary_index_offset = 0xDD8;

// These descriptors borrow engine buffers. An owning sdk::TArray snapshot
// would free the source/clone allocation when validation leaves scope.
using ViewArraySnapshot = sdk::TArrayLite<sdk::FSceneView*>;
static_assert(sizeof(ViewArraySnapshot) == 16);
static_assert(std::is_trivially_copyable_v<ViewArraySnapshot>);
static_assert(std::is_trivially_destructible_v<ViewArraySnapshot>);
static_assert(offsetof(ViewArraySnapshot, data) == 0 && offsetof(ViewArraySnapshot, count) == 8 &&
    offsetof(ViewArraySnapshot, capacity) == 12);

struct FamilyFunctions {
    uintptr_t copy{}, destroy{}, vtable{};
};

template<class Family, size_t Capacity, size_t Extent>
std::optional<size_t> link_families(std::span<Family*, Extent> original, Family* selected, Family* secondary,
    std::array<Family*, Capacity>& linked) {
    if (original.empty() || original.size() >= Capacity || selected == nullptr || secondary == nullptr ||
        selected == secondary || std::count(original.begin(), original.end(), selected) != 1 ||
        std::find(original.begin(), original.end(), secondary) != original.end()) { return {}; }
    size_t count{};
    for (auto* family : original) {
        if (family == nullptr) { return {}; }
        linked[count++] = family;
        if (family == selected) { linked[count++] = secondary; }
    }
    return count;
}

inline bool fresh_upscalers(const std::array<uintptr_t, 4>& interfaces) {
    return interfaces[1] == 0 && interfaces[2] == 0 && interfaces[3] == 0;
}

inline bool record_shared_interfaces(const std::array<uintptr_t, 4>& source,
    const std::array<uintptr_t, 4>& clone, std::array<uintptr_t, 4>& borrowed) {
    bool unique_upscalers = true;
    for (size_t index = 0; index < source.size(); ++index) {
        if (source[index] != 0 && source[index] == clone[index]) {
            borrowed[index] = source[index];
            if (index != 0) { unique_upscalers = false; }
        }
    }
    return unique_upscalers;
}

inline void release_borrowed_interfaces(void* family, const std::array<uintptr_t, 4>& borrowed) {
    for (size_t index = 0; index < borrowed.size(); ++index) {
        auto* slot = static_cast<uint8_t*>(family) + family_interfaces_offset + index * sizeof(uintptr_t);
        uintptr_t current{};
        std::memcpy(&current, slot, sizeof(current));
        if (borrowed[index] != 0 && current == borrowed[index]) {
            constexpr uintptr_t empty{};
            std::memcpy(slot, &empty, sizeof(empty));
        }
    }
}

inline std::optional<FamilyFunctions> family_contract(const sdk::discovery::Memory& memory, const Image& engine,
    uintptr_t copy, size_t copy_size, uintptr_t destroy, size_t destroy_size,
    uintptr_t eye_query, uintptr_t secondary_query) {
    if (copy_size != 0x442 || destroy_size != 0x110 ||
        !engine.contains(copy, copy_size) || !engine.contains(destroy, destroy_size)) { return {}; }
    constexpr auto copy_prefix = std::to_array<uint8_t>({
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x6C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,
        0x57,0x41,0x56,0x41,0x57,0x48,0x83,0xEC,0x40,0x48,0x8D,0x71,0x08,0x45,0x33,0xFF,0x4C,0x89,0x3E});
    constexpr auto copy_source = std::to_array<uint8_t>({0x48,0x89,0x01,0x48,0x8B,0xDA,0x48,0x63,0x6A,0x10});
    constexpr auto all_views = std::to_array<uint8_t>({
        0x48,0x8D,0x77,0x18,0x4C,0x89,0x3E,0x48,0x8D,0x46,0x0C,0x48,0x63,0x6B,0x20,0x4C,0x8B,0x73,0x18,0x89,0x6E,0x08});
    constexpr auto copy_tail = std::to_array<uint8_t>({
        0x0F,0xB6,0x83,0x90,0x01,0x00,0x00,0x48,0x8B,0x6C,0x24,0x68,0x48,0x8B,0x74,0x24,0x70,
        0x88,0x87,0x90,0x01,0x00,0x00,0x0F,0xB6,0x83,0x91,0x01,0x00,0x00,0x88,0x87,0x91,0x01,0x00,0x00,
        0x0F,0xB6,0x83,0x92,0x01,0x00,0x00,0x48,0x8B,0x5C,0x24,0x60,0x88,0x87,0x92,0x01,0x00,0x00,
        0x48,0x8B,0xC7,0x48,0x83,0xC4,0x40,0x41,0x5F,0x41,0x5E,0x5F,0xC3});
    constexpr auto destroy_prefix = std::to_array<uint8_t>({0x40,0x53,0x48,0x83,0xEC,0x20});
    constexpr auto destroy_source = std::to_array<uint8_t>({0x48,0x8B,0xD9,0x48,0x89,0x01});
    constexpr auto eye_pass = std::to_array<uint8_t>({0x83,0xB9,0xD0,0x0D,0x00,0x00,0x00,0x0F,0x95,0xC0,0xC3});
    constexpr auto view_family = std::to_array<uint8_t>({0x48,0x8B,0x41,0x08,0x44,0x8B,0x40,0x10});
    constexpr auto primary_index = std::to_array<uint8_t>({0x49,0x8B,0x6D,0x08,0x41,0x8B,0xBD,0xD8,0x0D,0x00,0x00,0xFF,0xC7,0x3B,0x7D,0x10});
    if (!code_matches(memory, engine, copy, copy_prefix) || !code_matches(memory, engine, copy + 0x29, copy_source) ||
        !code_matches(memory, engine, copy + 0x7C, all_views) || !code_matches(memory, engine, copy + 0x3FF, copy_tail) ||
        !code_matches(memory, engine, destroy, destroy_prefix) || !code_matches(memory, engine, destroy + 0xD, destroy_source) ||
        !code_matches(memory, engine, eye_query, eye_pass) || !engine.contains(secondary_query, 0x81) ||
        !code_matches(memory, engine, secondary_query + 0x29, view_family) ||
        !code_matches(memory, engine, secondary_query + 0x71, primary_index)) { return {}; }

    const auto vtable_from_lea = [&](uintptr_t instruction) -> std::optional<uintptr_t> {
        std::array<uint8_t, 7> bytes{};
        if (!engine.contains(instruction, bytes.size()) || !memory.load(instruction, bytes) ||
            bytes[0] != 0x48 || bytes[1] != 0x8D || bytes[2] != 0x05) { return {}; }
        int32_t displacement{};
        std::memcpy(&displacement, bytes.data() + 3, sizeof(displacement));
        const auto result = sdk::discovery::relative_address(instruction + bytes.size(), displacement);
        return result && engine.contains(*result, sizeof(uintptr_t)) && *result % alignof(uintptr_t) == 0 ? result : std::nullopt;
    };
    const auto table = vtable_from_lea(copy + 0x22);
    if (!table || vtable_from_lea(destroy + 6) != table) { return {}; }
    uintptr_t deleting_destructor{};
    if (!memory.load(*table, deleting_destructor) || !engine.contains(deleting_destructor, 1) ||
        !memory.executable || !memory.executable(memory.context, deleting_destructor, 1)) { return {}; }
    for (size_t index = 0; index < 4; ++index) {
        const uint32_t offset = static_cast<uint32_t>(family_interfaces_offset + index * sizeof(uintptr_t));
        std::array<uint8_t, 14> copied{0x48,0x8B,0x83,0,0,0,0,0x48,0x89,0x87,0,0,0,0};
        std::memcpy(copied.data() + 3, &offset, sizeof(offset));
        std::memcpy(copied.data() + 10, &offset, sizeof(offset));
        std::array<uint8_t, 7> destroyed{0x48,0x8B,static_cast<uint8_t>(index == 0 ? 0x89 : 0x8B),0,0,0,0};
        std::memcpy(destroyed.data() + 3, &offset, sizeof(offset));
        if (!code_matches(memory, engine, copy + 0x379 + index * copied.size(), copied) ||
            !code_matches(memory, engine, destroy + 0x13 + index * 0x16, destroyed)) { return {}; }
    }
    return FamilyFunctions{copy, destroy, *table};
}

inline bool validated_console_variable(const sdk::discovery::Memory& memory, const Image& core,
    uintptr_t object, ConsoleType type) {
    uintptr_t table{};
    std::array<uintptr_t, 26> slots{};
    if (object < 0x10000 || object % alignof(uintptr_t) != 0 || !memory.load(object, table) ||
        table % alignof(uintptr_t) != 0 || !core.contains(table, sizeof(slots)) || !memory.load(table, slots)) { return false; }

    const auto type_slot = type == ConsoleType::Floating ? 10U : type == ConsoleType::Boolean ? 8U : 9U;
    for (const auto index : {0U, 7U, type_slot, 16U, 20U, 21U, 24U, 25U}) {
        if (!core.contains(slots[index], 1) || !memory.executable ||
            !memory.executable(memory.context, slots[index], 1)) { return false; }
    }

    // Prove the casts from instructions, never execute an unknown AsVariable/AsCommand.
    // Reject strings, shadows, shifted tables and non-Core implementations.
    constexpr std::array<uint8_t, 4> returns_this{0x48, 0x8B, 0xC1, 0xC3};
    constexpr std::array<uint8_t, 3> returns_null{0x33, 0xC0, 0xC3};
    constexpr std::array<uint8_t, 3> returns_true{0xB0, 0x01, 0xC3};
    // Matching UE5.6.1 Core PDB: numeric/ref Set saves the value in RSI and
    // SetBy from R8D in EDI. No guessed raw TConsoleVariableData is published.
    constexpr std::array<uint8_t, 24> tagged_set_prefix{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,
        0x41,0x8B,0xF8,0x48,0x8B,0xF2,0x48,0x8B,0xD9};
    return code_matches(memory, core, slots[7], returns_this) &&
        code_matches(memory, core, slots[16], returns_null) &&
        code_matches(memory, core, slots[type_slot], returns_true) &&
        code_matches(memory, core, slots[21], tagged_set_prefix);
}

inline bool one_family_wrapper(std::span<const uint8_t> bytes, uintptr_t begin,
    uintptr_t callee, uintptr_t caller_return) {
    if (bytes.empty() || bytes.size() > 0x180 || begin > (std::numeric_limits<uintptr_t>::max)() - bytes.size()) { return false; }
    std::optional<uint8_t> count_offset, pointer_offset, element_offset, element_address, descriptor_offset;
    bool called{};
    size_t offset{};
    // Validate instruction boundaries and the one-element stack array passed
    // indirectly in R8, plus the exact direct-call return. No broad DLL fallback.
    for (uint32_t instructions = 0; offset < bytes.size() && instructions < 64; ++instructions) {
        INSTRUX ix{};
        const auto* b = bytes.data() + offset;
        if (!ND_SUCCESS(NdDecodeEx(&ix, b, bytes.size() - offset, ND_CODE_64, ND_DATA_64)) ||
            ix.Length == 0 || ix.Length > bytes.size() - offset) { return false; }
        if (!called && ix.Length == 8 && b[0] == 0xC7 && b[1] == 0x44 && b[2] == 0x24) {
            if (b[4] != 1 || b[5] != 0 || b[6] != 0 || b[7] != 0) { return false; }
            count_offset = b[3];
        } else if (!called && ix.Length == 5 && b[0] == 0x48 && b[1] == 0x8D && b[2] == 0x44 && b[3] == 0x24) {
            element_address = b[4];
        } else if (!called && ix.Length == 5 && b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x44 && b[3] == 0x24) {
            pointer_offset = b[4];
        } else if (!called && ix.Length == 5 && b[0] == 0x4C && b[1] == 0x89 && b[2] == 0x44 && b[3] == 0x24) {
            element_offset = b[4];
        } else if (!called && ix.Length == 5 && b[0] == 0x4C && b[1] == 0x8D && b[2] == 0x44 && b[3] == 0x24) {
            descriptor_offset = b[4];
        } else if (ix.Instruction == ND_INS_CALLNR) {
            if (called || ix.Length != 5 || b[0] != 0xE8 || !count_offset || !pointer_offset ||
                !element_offset || !element_address || !descriptor_offset ||
                unsigned(*count_offset) != unsigned(*descriptor_offset) + 8 || *pointer_offset != *descriptor_offset ||
                *element_offset != *element_address) { return false; }
            int32_t displacement{};
            std::memcpy(&displacement, b + 1, sizeof(displacement));
            const auto next = begin + offset + ix.Length;
            const auto target = sdk::discovery::relative_address(next, displacement);
            if (next != caller_return || !target || *target != callee) { return false; }
            called = true;
        } else if (ix.Instruction == ND_INS_RETN) {
            return called && ix.Length == 1 && b[0] == 0xC3;
        } else if (ix.BranchInfo.IsBranch) {
            return false;
        }
        offset += ix.Length;
    }
    return false;
}

} // namespace uevr::satisfactory
