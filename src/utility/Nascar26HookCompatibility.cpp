#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

#include <safetyhook/os.hpp>
#include <spdlog/spdlog.h>
#include <utility/ScopeGuard.hpp>
#include "Nascar26HookCompatibility.hpp"
#include "Logging.hpp"

namespace uevr::nascar26 {
namespace {
std::once_flag g_once;
std::atomic_bool g_target{};
std::atomic_bool g_validated{};
uintptr_t g_image{};
size_t g_image_size{};

bool preserve_image(uint8_t* address, size_t bytes, uint32_t protection, uint32_t* old) {
    if (intersects_image(reinterpret_cast<uintptr_t>(address), bytes, g_image, g_image_size)) {
        SPDLOG_ERROR_ONCE("[NASCAR26][CodePreserving] Refused a game-image write; checked instructions and vtable data remain unchanged");
    }
    return protect_external_memory(address, bytes, protection, old, g_image, g_image_size);
}

bool known_file(const wchar_t* filename) {
    constexpr std::array<uint8_t, 32> expected{
        0xfc,0x57,0xa3,0xc3,0xf5,0xae,0x06,0x3d,0xaa,0x9e,0x43,0x46,0xf5,0x51,0xa1,0xb6,
        0x1e,0xb7,0x41,0x78,0x4a,0xa0,0x00,0x41,0x11,0xb1,0x30,0x3c,0x4d,0x2f,0x30,0xbb};
    std::ifstream input{std::filesystem::path{filename}, std::ios::binary};
    if (!input) { return false; }
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) { return false; }
    utility::ScopeGuard close_algorithm{[&] { BCryptCloseAlgorithmProvider(algorithm, 0); }};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { return false; }
    utility::ScopeGuard close_hash{[&] { BCryptDestroyHash(hash); }};
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0) < 0) {
            return false;
        }
    }
    if (!input.eof()) { return false; }
    std::array<uint8_t, 32> actual{};
    return BCryptFinishHash(hash, actual.data(), static_cast<ULONG>(actual.size()), 0) >= 0 && actual == expected;
}

bool image_page(uintptr_t address, size_t bytes, uintptr_t image, bool code) {
    MEMORY_BASIC_INFORMATION info{};
    constexpr DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Type == MEM_IMAGE && reinterpret_cast<uintptr_t>(info.AllocationBase) == image &&
        !(info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && bool(info.Protect & execute) == code &&
        address >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
        bytes <= info.RegionSize - (address - reinterpret_cast<uintptr_t>(info.BaseAddress));
}
}

bool protect_external_memory(uint8_t* address, size_t bytes, uint32_t protection,
    uint32_t* old, uintptr_t image, size_t image_size) {
    if (!old || intersects_image(reinterpret_cast<uintptr_t>(address), bytes, image, image_size)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    DWORD previous{};
    if (!VirtualProtect(address, bytes, protection, &previous)) { return false; }
    *old = previous;
    return true;
}

bool read_memory(uintptr_t address, void* out, size_t bytes) {
    SIZE_T got{};
    return address && out && bytes && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), out, bytes, &got) && got == bytes;
}

std::optional<NativeFamilyTargets> read_native_family_targets(uintptr_t family) {
    NativeFamilyTargets targets{};
    if (!family || family > UINTPTR_MAX - native_family_targets_offset - sizeof(targets) ||
        !read_memory(family + native_family_targets_offset, &targets, sizeof(targets))) { return {}; }
    return targets;
}

bool matches_code(uintptr_t address, std::span<const uint8_t> expected) {
    std::array<uint8_t, 128> actual{};
    return !expected.empty() && expected.size() <= actual.size() &&
        image_page(address, expected.size(), g_image, true) &&
        read_memory(address, actual.data(), expected.size()) &&
        std::equal(expected.begin(), expected.end(), actual.begin());
}

bool native_owned_object_valid(const NativeOwnedObject& owner, uintptr_t item, bool require_root) {
    struct Item { uint64_t flags_and_refcount; uintptr_t object; } before{}, after{};
    struct Header { uintptr_t vtable; uint32_t flags; int32_t index; } header{};
    const auto item_matches = [&](const Item& observed) {
        const auto flags = static_cast<uint32_t>(observed.flags_and_refcount >> 32);
        constexpr uint32_t invalid_flags = (1u << 21) | (1u << 28) | (1u << 31);
        return observed.object == owner.object && !(flags & invalid_flags) &&
            (!require_root || (flags & native_owner_root_flag));
    };
    // Check the cached array index first. Never adopt or dereference a reused owner.
    return owner.object && owner.vtable && owner.index >= 0 && item &&
        read_memory(item, &before, sizeof(before)) && item_matches(before) &&
        read_memory(owner.object, &header, sizeof(header)) &&
        header.vtable == owner.vtable && header.index == owner.index &&
        read_memory(item, &after, sizeof(after)) && item_matches(after);
}

bool validate_native_rooting() {
    if (!is_validated_build()) { return false; }
    const auto code = [](uintptr_t rva, std::initializer_list<uint8_t> bytes) {
        return matches_code(g_image + rva, {bytes.begin(), bytes.size()});
    };
    // UE5.7 RootSet writes must notify the GC root registry and incremental barrier.
    // Validate the native flag setter and both downstream calls; do not patch them.
    return code(native_owner_set_flags_rva, {0x40,0x53,0x48,0x83,0xec,0x20,0x81,0xe2,0xff,0x3f,0xfe,0xef,
            0x48,0x8b,0xd9,0xf7,0xc2,0x00,0x00,0x50,0x4e,0x74,0x73}) &&
        code(0x150e6a0, {0x4c,0x8b,0x0b,0x4d,0x8b,0xc1,0x49,0xc1,0xf8,0x20,0x41,0x8b,0xc0,0x23,0xc2,
            0x3b,0xc2,0x74,0x36,0x48,0x63,0xc2,0x49,0x63,0xc8,0x48,0x0b,0xc8,0x49,0x63,0xc1,
            0x48,0xc1,0xe1,0x20,0x48,0x0b,0xc8,0x49,0x8b,0xc1,0xf0,0x48,0x0f,0xb1,0x0b,0x75,0xd0,
            0x40,0xb7,0x01,0x41,0xf7,0xc0,0x00,0x00,0x50,0x4e,0x75,0x0d,0x45,0x85,0xc9,0x75,0x08,
            0x48,0x8b,0xcb,0xe8,0x97,0x31,0xff,0xff,0x40,0x84,0x3d,0x09,0x1c,0xbe,0x08,0x74,0x09,
            0x48,0x8b,0x4b,0x08,0xe8,0x15,0x2d,0xff,0xff}) &&
        code(0x1501880, {0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0x41,0x08,
            0x8b,0x70,0x0c,0x0f,0xb6,0x05,0xe8,0xeb,0xbe,0x08}) &&
        code(0x15018e0, {0x8b,0xce,0xe8,0xd9,0x1e,0x00,0x00});
}

void initialize() {
    std::call_once(g_once, [] {
        std::array<wchar_t, 32768> path{};
        const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!n || n >= path.size() || !matches_name({path.data(), n})) { return; }
        g_target.store(true, std::memory_order_release);
        g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read_memory(g_image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
            !read_memory(g_image + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 0x40000000) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Invalid image header; stereo hook initialization refused");
            return;
        }
        g_image_size = nt.OptionalHeader.SizeOfImage;
        if (safetyhook::has_protection_override()) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Existing protection backend; refusing to replace it");
            return;
        }
        safetyhook::set_protection_override(preserve_image);
        DWORD ignored{};
        const auto version_size = GetFileVersionInfoSizeW(path.data(), &ignored);
        if (version_size == 0 || version_size > 1024 * 1024) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Invalid version data; adapters remain disabled");
            return;
        }
        std::vector<uint8_t> version(version_size);
        VS_FIXEDFILEINFO* info{};
        UINT info_size{};
        const bool version_ok = version_size && GetFileVersionInfoW(path.data(), 0, version_size, version.data()) &&
            VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&info), &info_size) &&
            info && info_size >= sizeof(*info) && info->dwSignature == 0xfeef04bd &&
            matches_build_metadata(info->dwFileVersionMS, info->dwFileVersionLS, nt.FileHeader.TimeDateStamp, g_image_size);
        if (!version_ok || !known_file(path.data())) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Unvalidated game build; no game-code patches or virtual adapters enabled");
            return;
        }
        constexpr std::array<uint8_t, 23> slate_read{
            0x48,0x8b,0x03,0x48,0x8b,0xcb,0xff,0x50,0x18,0x4c,0x8b,0x70,0x08,
            0x4d,0x85,0xf6,0x74,0x05,0xf0,0x41,0xff,0x46,0x08};
        if (!matches_code(g_image + slate_getter_return_rva - 9, slate_read)) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Slate getter caller differs; adapters remain disabled");
            return;
        }
        constexpr std::array<uint8_t, 29> native_read{
            0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x81,0xd0,0x00,0x00,0x00,0x33,0xdb,
            0x48,0x85,0xc0,0x74,0x09,0x48,0x8b,0x58,0x20,0x48,0x85,0xdb,0x75,0x21};
        if (!matches_code(g_image + 0x2dd5c00, native_read)) {
            SPDLOG_ERROR("[NASCAR26][CodePreserving] Texture resource chain differs; adapters remain disabled");
            return;
        }
        g_validated.store(true, std::memory_order_release);
        SPDLOG_INFO("[NASCAR26][CodePreserving] Validated UE5.7.4 build and Slate caller; Native/UI object adapters are experimental, image patches refused");
    });
}

bool is_target() { return g_target.load(std::memory_order_acquire); }
bool is_validated_build() { return g_validated.load(std::memory_order_acquire); }
uintptr_t image_base() { return g_image; }

uintptr_t find_virtual_slot(uintptr_t table, uintptr_t expected, size_t count) {
    return is_validated_build() ? find_virtual_slot_in_image(table, expected, count, g_image) : 0;
}

uintptr_t find_virtual_slot_in_image(uintptr_t table, uintptr_t expected, size_t count, uintptr_t image) {
    if (!image || !count || count > 192 || table % sizeof(uintptr_t) ||
        !image_page(table, count * sizeof(uintptr_t), image, false) ||
        !image_page(expected, 1, image, true)) { return 0; }
    uintptr_t found{};
    for (size_t i = 0; i < count; ++i) {
        uintptr_t current{};
        if (!read_memory(table + i * sizeof(uintptr_t), &current, sizeof(current))) { return 0; }
        if (current == expected) {
            if (found) { return 0; }
            found = table + i * sizeof(uintptr_t);
        }
    }
    return found;
}

bool writable_object_pointer(uintptr_t address) {
    MEMORY_BASIC_INFORMATION info{};
    return address && address % sizeof(uintptr_t) == 0 &&
        VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.Protect == PAGE_READWRITE &&
        address >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
        sizeof(uintptr_t) <= info.RegionSize - (address - reinterpret_cast<uintptr_t>(info.BaseAddress));
}

ObjectVTable::~ObjectVTable() {
    reset();
    // Retain one immutable page per adapter until process exit: a dispatched call
    // can still hold the table after reset. Production creates at most six adapters.
}

bool ObjectVTable::prepare(uintptr_t table, size_t count, std::span<const SlotPatch> patches, uintptr_t image) {
    std::scoped_lock lock{m_mutex};
    if (!image || !count || count > m_original.size() || patches.empty() || patches.size() > count ||
        table < sizeof(uintptr_t) || table % sizeof(uintptr_t)) { return false; }
    if (prepared()) {
        if (table != m_table || count != m_count || patches.size() != m_patch_count) { return false; }
        std::array<bool, 256> used{};
        for (const auto& patch : patches) {
            if (patch.index >= count || used[patch.index] || m_original[patch.index] != patch.expected ||
                m_shadow[patch.index] != patch.replacement) { return false; }
            used[patch.index] = true;
        }
        return true;
    }
    if (!image_page(table - sizeof(uintptr_t), (count + 1) * sizeof(uintptr_t), image, false)) { return false; }
    std::array<uintptr_t, 257> copy{};
    if (!read_memory(table - sizeof(uintptr_t), copy.data(), (count + 1) * sizeof(uintptr_t))) { return false; }
    for (size_t i = 0; i < count; ++i) {
        if (!image_page(copy[i + 1], 1, image, true)) { return false; }
    }
    std::array<bool, 256> used{};
    constexpr DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    for (const auto& patch : patches) {
        MEMORY_BASIC_INFORMATION info{};
        if (patch.index >= count || used[patch.index] || copy[patch.index + 1] != patch.expected ||
            !patch.replacement || patch.expected == patch.replacement ||
            VirtualQuery(reinterpret_cast<void*>(patch.replacement), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || !(info.Protect & execute) || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) { return false; }
        used[patch.index] = true;
    }
    auto* const storage = static_cast<uintptr_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!storage) { return false; }
    std::copy_n(copy.data(), count + 1, storage);
    for (const auto& patch : patches) { storage[patch.index + 1] = patch.replacement; }
    DWORD old{};
    if (!VirtualProtect(storage, 4096, PAGE_READONLY, &old)) {
        VirtualFree(storage, 0, MEM_RELEASE);
        return false;
    }
    std::copy_n(copy.data() + 1, count, m_original.data());
    m_table = table;
    m_count = count;
    m_primary_slot = patches.front().index;
    m_patch_count = patches.size();
    m_shadow = storage + 1;
    m_prepared.store(true, std::memory_order_release);
    return true;
}

bool ObjectVTable::owns(uintptr_t object) const {
    uintptr_t value{};
    return prepared() && object && object == slot_address() &&
        read_memory(object, &value, sizeof(value)) && value == reinterpret_cast<uintptr_t>(m_shadow);
}

bool validate_synced_redraw() {
    if (!is_validated_build()) { return false; }
    // UE5.7.4 FViewport::Draw(bool), reached by UGameEngine::RedrawViewports.
    // Validate only: unlike the shared Synced path, no inline hook is installed.
    constexpr std::array<uint8_t, 32> draw{
        0x40,0x55,0x56,0x41,0x55,0x41,0x57,0x48,0x8d,0xac,0x24,0x38,0xff,0xff,0xff,0x48,
        0x81,0xec,0xc8,0x01,0x00,0x00,0x48,0x8b,0x05,0xb3,0x89,0xdf,0x05,0x48,0x33,0xc4};
    constexpr std::array<uint8_t, 5> caller{0xe8,0xae,0xb1,0x68,0x00};
    constexpr std::array<uint8_t, 9> world_thunk{0x48,0x83,0xe9,0x28,0xe9,0x67,0xc3,0x4e,0xfe};
    constexpr std::array<uint8_t, 5> world_getter{0x48,0x8b,0x41,0x78,0xc3};
    uintptr_t world_slot{};
    return find_virtual_slot(g_image + engine_vtable_rva, g_image + 0x3b2f310, engine_slots) ==
            g_image + engine_vtable_rva + 0x4e0 &&
        read_memory(g_image + viewport_dispatch_vtable_rva + 0x30, &world_slot, sizeof(world_slot)) &&
        world_slot == g_image + 0x3b61e60 && matches_code(g_image + 0x3b61e60, world_thunk) &&
        matches_code(g_image + 0x204e1d0, world_getter) &&
        matches_code(g_image + 0x3b2f37d, caller) && matches_code(g_image + viewport_draw_rva, draw);
}

bool validate_ghost_view_setup() {
    if (!is_validated_build()) { return false; }
    const auto code = [](uintptr_t rva, std::initializer_list<uint8_t> bytes) {
        return matches_code(g_image + rva, {bytes.begin(), bytes.size()});
    };
    return find_virtual_slot(g_image + localplayer_vtable_rva, g_image + init_options_rva, localplayer_slots) ==
            g_image + localplayer_vtable_rva + 91 * sizeof(uintptr_t) &&
        find_virtual_slot(g_image + localplayer_vtable_rva, g_image + calc_scene_view_rva, localplayer_slots) ==
            g_image + localplayer_vtable_rva + 92 * sizeof(uintptr_t) &&
        find_virtual_slot(g_image + localplayer_vtable_rva, g_image + projection_data_rva, localplayer_slots) ==
            g_image + localplayer_vtable_rva + 109 * sizeof(uintptr_t) &&
        code(init_options_rva, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c,0x24,0x18,0x56,0x57,0x41,0x55,0x41,0x56}) &&
        code(projection_data_rva, {0x40,0x55,0x56,0x57,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24,0xf8,0xf5}) &&
        code(calc_scene_view_rva, {0x40,0x55,0x53,0x48,0x8d,0xac,0x24,0xd8,0xec,0xff,0xff,0xb8,0x28,0x14,0x00,0x00}) &&
        code(0x3b58903, {0x4c,0x8b,0x90,0xe0,0x02,0x00,0x00}) &&
        code(calc_scene_view_return_rva - 3, {0x41,0xff,0xd2}) &&
        code(0x3cc8975, {0x4c,0x8b,0x90,0xd8,0x02,0x00,0x00}) &&
        code(init_options_return_rva - 3, {0x41,0xff,0xd2}) &&
        code(projection_data_return_rva - 6, {0xff,0x90,0x68,0x03,0x00,0x00}) &&
        code(0x3cc9467, {0x4c,0x8d,0xb7,0xd0,0x00,0x00,0x00}) &&
        code(0x3cc94b7, {0x4c,0x6b,0xe0,0x38}) &&
        code(0x3cc94ec, {0xe8,0x7f,0x12,0x33,0x00}) &&
        code(0x3cc94f4, {0x49,0x8b,0x4c,0x04,0x08,0x48,0x89,0x8e,0x60,0x01,0x00,0x00}) &&
        code(0x3cc9457, {0x89,0x86,0xc0,0x01,0x00,0x00}) &&
        code(0x3cc954d, {0x89,0x9e,0xc4,0x01,0x00,0x00}) &&
        code(0x3ff45d1, {0x48,0x8b,0x82,0x60,0x01,0x00,0x00,0x48,0x89,0x41,0x10}) &&
        code(0x3ff4b29, {0x89,0x83,0xd4,0x0d,0x00,0x00});
}

bool ObjectVTable::image_unchanged() const {
    std::array<uintptr_t, 256> current{};
    return prepared() && read_memory(m_table, current.data(), m_count * sizeof(uintptr_t)) &&
        std::equal(m_original.begin(), m_original.begin() + m_count, current.begin());
}

bool validate_native_renderer() {
    if (!validate_ghost_view_setup() || !validate_native_rooting()) { return false; }
    const auto code = [](uintptr_t rva, std::initializer_list<uint8_t> bytes) {
        return matches_code(g_image + rva, {bytes.begin(), bytes.size()});
    };
    return find_virtual_slot(g_image + renderer_vtable_rva, g_image + renderer_single_rva, renderer_slots) ==
            g_image + renderer_vtable_rva + 8 * sizeof(uintptr_t) &&
        code(renderer_single_rva, {0x48,0x83,0xec,0x38,0xc7,0x44,0x24,0x28,0x01,0x00,0x00,0x00}) &&
        code(0x2b09dfb, {0xe8,0x80,0xf8,0xff,0xff}) &&
        code(renderer_plural_rva, {0x48,0x89,0x5c,0x24,0x08,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57}) &&
        code(0x2b096d4, {0x48,0x8b,0x48,0x40,0x48,0x85,0xc9}) &&
        code(0x3b59683, {0x4c,0x8b,0x49,0x40,0x4c,0x8d,0x85,0x30,0x02,0x00,0x00}) &&
        code(renderer_return_rva - 3, {0x41,0xff,0xd1}) &&
        code(family_copy_rva + 0x1e, {0x48,0x8d,0x71,0x08,0x45,0x33,0xe4,0x4c,0x89,0x26}) &&
        code(family_copy_rva + 0x28, {0x48,0x8d,0x05,0xe1,0xdf,0x6b,0x04,0x48,0x89,0x01,0x48,0x8b,0xfa,0x48,0x63,0x6a,0x10}) &&
        code(0x3ff5da1, {0x48,0x8b,0x47,0x30,0x48,0x89,0x43,0x30,0x48,0x8b,0x47,0x38,0x48,0x89,0x43,0x38,
            0x48,0x8b,0x47,0x40,0x48,0x89,0x43,0x40}) &&
        code(family_delete_rva, {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x8b,0xda,0x48,0x8b,0xf9,
            0xe8,0xdc,0xd4,0xff,0xff,0xf6,0xc3,0x01,0x74,0x0d,0xba,0x98,0x01,0x00,0x00}) &&
        code(0x3ff492b, {0x48,0x8b,0x82,0x48,0x01,0x00,0x00,0x48,0x89,0x81,0x80,0x03,0x00,0x00}) &&
        code(0x3ff4947, {0x48,0x8b,0x82,0x20,0x01,0x00,0x00,0x48,0x89,0x81,0x90,0x03,0x00,0x00}) &&
        code(0x4138680, {0x83,0xb9,0xd0,0x0d,0x00,0x00,0x02,0x0f,0x94,0xc0,0xc3});
}

bool ObjectVTable::install(uintptr_t object) {
    std::scoped_lock lock{m_mutex};
    if (!image_unchanged() || !writable_object_pointer(object)) { return false; }
    if (active()) { return owns(object); }
    if (slot_address() != 0) { return false; }
    uintptr_t value{};
    if (!read_memory(object, &value, sizeof(value)) || value != m_table) { return false; }
    m_object.store(object, std::memory_order_release);
    if (InterlockedCompareExchangePointer(reinterpret_cast<void**>(object), m_shadow,
        reinterpret_cast<void*>(m_table)) != reinterpret_cast<void*>(m_table)) {
        m_object.store(0, std::memory_order_release);
        return false;
    }
    m_active.store(true, std::memory_order_release);
    return true;
}

bool ObjectVTable::reset() {
    std::scoped_lock lock{m_mutex};
    const auto object = slot_address();
    if (!object) { return true; }
    if (!writable_object_pointer(object)) { return false; }
    InterlockedCompareExchangePointer(reinterpret_cast<void**>(object), reinterpret_cast<void*>(m_table), m_shadow);
    m_active.store(false, std::memory_order_release);
    m_object.store(0, std::memory_order_release);
    return true;
}
}
