#define NOMINMAX
#include <Windows.h>
#include <winternl.h>

#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include <spdlog/spdlog.h>
#include <safetyhook/os.hpp>
#include <utility/ScopeGuard.hpp>
#include "utility/Logging.hpp"

#include "HiFiRushHookMemory.hpp"
#include "ProtectedHookTrampoline.hpp"

namespace uevr::hifi {
namespace {
using NtProtectMemory = NTSTATUS (NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
struct ValidatedProtection {
    uintptr_t image_base{};
    size_t image_size{};
    uint8_t* entry{};
    uint8_t* trampoline{};
    std::array<uint8_t, 13> trampoline_bytes{};
    std::array<uint8_t, 32> entry_bytes{};
    NtProtectMemory original{};
} g_protection;

bool read_memory(uintptr_t address, void* buffer, size_t size) {
    SIZE_T bytes{};
    return address != 0 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address),
        buffer, size, &bytes) && bytes == size;
}

std::optional<uintptr_t> find_hbk_version_marker(uintptr_t base) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_memory(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !read_memory(base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        !nt.FileHeader.NumberOfSections || nt.FileHeader.NumberOfSections > 96 ||
        !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 0x40000000) {
        return std::nullopt;
    }
    const auto table = static_cast<size_t>(dos.e_lfanew) + sizeof(nt);
    if (table + nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER) > nt.OptionalHeader.SizeOfHeaders ||
        nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) {
        return std::nullopt;
    }
    // The licensee branch string is real engine evidence; the retail file version is not (0.3743...).
    constexpr size_t step = 64 * 1024;
    std::vector<uint8_t> buffer(step + sizeof(hbk_ue427_branch));
    for (size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!read_memory(base + table + i * sizeof(section), &section, sizeof(section))) {
            return std::nullopt;
        }
        if (!(section.Characteristics & IMAGE_SCN_MEM_READ) ||
            (section.Characteristics & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)) ||
            !section.Misc.VirtualSize || section.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
            section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - section.VirtualAddress) {
            continue;
        }
        const auto end = base + section.VirtualAddress + section.Misc.VirtualSize;
        for (auto cursor = base + section.VirtualAddress; cursor < end;) {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) != sizeof(info)) {
                break;
            }
            const auto region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (region_end <= cursor) {
                break;
            }
            const auto readable = info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
                reinterpret_cast<uintptr_t>(info.AllocationBase) == base && info.Protect == PAGE_READONLY;
            if (!readable) {
                cursor = std::min(end, region_end);
                continue;
            }
            const auto available = std::min(end, region_end) - cursor;
            const auto bytes = std::min(buffer.size(), available);
            if (read_memory(cursor, buffer.data(), bytes)) {
                if (const auto offset = find_hbk_ue427_branch(std::span{buffer}.first(bytes))) {
                    return cursor + *offset;
                }
            }
            cursor += std::min(step, available);
        }
    }
    return std::nullopt;
}

bool executable(uintptr_t address, size_t size, DWORD type = 0) {
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) == sizeof(info) &&
        info.State == MEM_COMMIT && (type == 0 || info.Type == type) &&
        !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
        address >= reinterpret_cast<uintptr_t>(info.BaseAddress) &&
        size <= info.RegionSize - (address - reinterpret_cast<uintptr_t>(info.BaseAddress));
}

bool matches_access(DWORD actual, DWORD requested) {
    if (actual == requested) {
        return true;
    }
    // Image pages can report copy-on-write instead of ordinary writable protection.
    return (actual == PAGE_WRITECOPY && requested == PAGE_READWRITE) ||
        (actual == PAGE_EXECUTE_WRITECOPY && requested == PAGE_EXECUTE_READWRITE);
}

bool range_has_access(uintptr_t address, size_t size, DWORD protection) {
    for (auto cursor = address; cursor < address + size;) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof(info)) != sizeof(info) ||
            info.State != MEM_COMMIT || !matches_access(info.Protect, protection)) {
            return false;
        }
        const auto end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (end <= cursor) {
            return false;
        }
        cursor = end;
    }
    return true;
}

bool protect_memory(uint8_t* address, size_t size, uint32_t protection, uint32_t* previous) {
    if (!size || !previous || reinterpret_cast<uintptr_t>(address) > UINTPTR_MAX - size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    const auto value = reinterpret_cast<uintptr_t>(address);
    MEMORY_BASIC_INFORMATION owner{};
    const auto game_image = VirtualQuery(address, &owner, sizeof(owner)) == sizeof(owner) &&
        owner.AllocationBase == GetModuleHandleW(nullptr);
    bool succeeded{};
    LONG status{};
    ULONG old_protection{};
    if (game_image) {
        if (!g_protection.original) {
            SetLastError(ERROR_NOT_SUPPORTED);
            return false;
        }
        std::array<uint8_t, 13> trampoline{};
        std::array<uint8_t, 32> entry{};
        if (value < g_protection.image_base || value - g_protection.image_base >= g_protection.image_size ||
            size > g_protection.image_size - (value - g_protection.image_base) ||
            size > owner.RegionSize - (value - reinterpret_cast<uintptr_t>(owner.BaseAddress)) ||
            !executable(reinterpret_cast<uintptr_t>(g_protection.trampoline), trampoline.size(), MEM_PRIVATE) ||
            !read_memory(reinterpret_cast<uintptr_t>(g_protection.trampoline), trampoline.data(), trampoline.size()) ||
            !read_memory(reinterpret_cast<uintptr_t>(g_protection.entry), entry.data(), entry.size()) ||
            trampoline != g_protection.trampoline_bytes || entry != g_protection.entry_bytes) {
            SPDLOG_ERROR_ONCE("[HiFiRush][HookMemory] Validated original protection stub changed; refusing game-code write");
            SetLastError(ERROR_INVALID_DATA);
            return false;
        }
        auto aligned_address = static_cast<PVOID>(address);
        auto aligned_size = static_cast<SIZE_T>(size);
        status = g_protection.original(GetCurrentProcess(), &aligned_address, &aligned_size, protection, &old_protection);
        succeeded = status >= 0;
    } else {
        succeeded = VirtualProtect(address, size, protection, &old_protection) != FALSE;
    }
    auto error = succeeded ? ERROR_SUCCESS : game_image ? ERROR_ACCESS_DENIED : GetLastError();
    if (succeeded && !range_has_access(value, size, protection)) {
        // A successful API return is insufficient. Roll back before reporting a rejected write.
        if (!range_has_access(value, size, old_protection)) {
            ULONG ignored{};
            if (game_image) {
                auto base = static_cast<PVOID>(address);
                auto bytes = static_cast<SIZE_T>(size);
                g_protection.original(GetCurrentProcess(), &base, &bytes, old_protection, &ignored);
            } else {
                VirtualProtect(address, size, old_protection, &ignored);
            }
            if (!range_has_access(value, size, old_protection)) {
                SPDLOG_ERROR_ONCE("[HiFiRush][HookMemory] Could not restore protection after failed readback");
            }
        }
        succeeded = false;
        error = ERROR_INVALID_ACCESS;
    }
    if (!succeeded) {
        static std::atomic_uint failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16) {
            SPDLOG_ERROR("[HiFiRush][HookMemory] Protection rejected at {:x}, size={}, requested={:x}, "
                         "NTSTATUS={:08x}, Win32={}; no hook write permitted", value, size, protection,
                static_cast<uint32_t>(status), error);
        }
        SetLastError(error ? error : ERROR_ACCESS_DENIED);
        return false;
    }
    *previous = old_protection;
    if (game_image) {
        SPDLOG_INFO_ONCE("[HiFiRush][HookMemory] Verified writable game code through retained Windows protection stub");
    }
    return true;
}

bool resolve_protection() {
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto entry = reinterpret_cast<uint8_t*>(GetProcAddress(ntdll, "NtProtectVirtualMemory"));
    if (!entry) {
        return false;
    }
    std::array<wchar_t, 32768> filename{};
    const auto length = GetModuleFileNameW(ntdll, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length >= filename.size()) {
        return false;
    }
    const auto file = CreateFileW(filename.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    utility::ScopeGuard close_file{[&] { CloseHandle(file); }};
    const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
    if (!mapping) {
        return false;
    }
    utility::ScopeGuard close_mapping{[&] { CloseHandle(mapping); }};
    const auto image = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!image) {
        return false;
    }
    utility::ScopeGuard unmap{[&] { UnmapViewOfFile(image); }};
    IMAGE_DOS_HEADER disk_dos{}, live_dos{};
    if (!read_memory(reinterpret_cast<uintptr_t>(image), &disk_dos, sizeof(disk_dos)) ||
        !read_memory(reinterpret_cast<uintptr_t>(ntdll), &live_dos, sizeof(live_dos)) ||
        disk_dos.e_magic != IMAGE_DOS_SIGNATURE || live_dos.e_magic != IMAGE_DOS_SIGNATURE ||
        disk_dos.e_lfanew <= 0 || disk_dos.e_lfanew > 0x1000 || disk_dos.e_lfanew != live_dos.e_lfanew) {
        return false;
    }
    IMAGE_NT_HEADERS64 disk_nt{}, live_nt{};
    if (!read_memory(reinterpret_cast<uintptr_t>(image) + disk_dos.e_lfanew, &disk_nt, sizeof(disk_nt)) ||
        !read_memory(reinterpret_cast<uintptr_t>(ntdll) + live_dos.e_lfanew, &live_nt, sizeof(live_nt)) ||
        disk_nt.Signature != IMAGE_NT_SIGNATURE || live_nt.Signature != IMAGE_NT_SIGNATURE ||
        disk_nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        disk_nt.FileHeader.TimeDateStamp != live_nt.FileHeader.TimeDateStamp ||
        disk_nt.OptionalHeader.SizeOfImage != live_nt.OptionalHeader.SizeOfImage) {
        return false;
    }
    const auto rva = reinterpret_cast<uintptr_t>(entry) - reinterpret_cast<uintptr_t>(ntdll);
    std::array<uint8_t, 32> original{}, current{};
    if (rva >= disk_nt.OptionalHeader.SizeOfImage || original.size() > disk_nt.OptionalHeader.SizeOfImage - rva ||
        !read_memory(reinterpret_cast<uintptr_t>(image) + rva, original.data(), original.size()) ||
        !read_memory(reinterpret_cast<uintptr_t>(entry), current.data(), current.size())) {
        return false;
    }
    if (current == original) {
        SPDLOG_INFO("[HiFiRush][HookMemory] Windows protection entry is unchanged; keeping ordinary hooks");
        return true;
    }
    if (current[0] != 0xe9 || std::memcmp(current.data() + 8, original.data() + 8, 24) != 0) {
        return false;
    }
    int32_t displacement{};
    std::memcpy(&displacement, current.data() + 1, sizeof(displacement));
    const auto relay = reinterpret_cast<uintptr_t>(entry) + 5 + static_cast<int64_t>(displacement);
    std::array<uint8_t, 14> relay_bytes{};
    if (!executable(relay, relay_bytes.size(), MEM_PRIVATE) || !read_memory(relay, relay_bytes.data(), relay_bytes.size()) ||
        relay_bytes[0] != 0xff || relay_bytes[1] != 0x25 ||
        relay_bytes[2] || relay_bytes[3] || relay_bytes[4] || relay_bytes[5]) {
        return false;
    }
    uintptr_t detour{};
    std::memcpy(&detour, relay_bytes.data() + 6, sizeof(detour));
    MEMORY_BASIC_INFORMATION detour_info{};
    const auto main_module = GetModuleHandleW(nullptr);
    if (!executable(detour, 1, MEM_IMAGE) ||
        VirtualQuery(reinterpret_cast<void*>(detour), &detour_info, sizeof(detour_info)) != sizeof(detour_info) ||
        detour_info.AllocationBase != main_module) {
        return false;
    }
    uintptr_t trampoline{};
    std::array<uint8_t, 13> candidate{};
    for (size_t distance = 13; distance <= 64 && distance <= relay; ++distance) {
        const auto address = relay - distance;
        if (!executable(address, candidate.size(), MEM_PRIVATE) || !read_memory(address, candidate.data(), candidate.size()) ||
            !matches_original_stub(candidate, address, original, reinterpret_cast<uintptr_t>(entry))) {
            continue;
        }
        if (trampoline) {
            return false;
        }
        trampoline = address;
    }
    IMAGE_DOS_HEADER game_dos{};
    IMAGE_NT_HEADERS64 game_nt{};
    if (!trampoline || !read_memory(reinterpret_cast<uintptr_t>(main_module), &game_dos, sizeof(game_dos)) ||
        game_dos.e_magic != IMAGE_DOS_SIGNATURE || game_dos.e_lfanew <= 0 || game_dos.e_lfanew > 0x1000 ||
        !read_memory(reinterpret_cast<uintptr_t>(main_module) + game_dos.e_lfanew, &game_nt, sizeof(game_nt)) ||
        game_nt.Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    g_protection.image_base = reinterpret_cast<uintptr_t>(main_module);
    g_protection.image_size = game_nt.OptionalHeader.SizeOfImage;
    g_protection.entry = entry;
    g_protection.entry_bytes = current;
    g_protection.trampoline = reinterpret_cast<uint8_t*>(trampoline);
    if (!read_memory(trampoline, g_protection.trampoline_bytes.data(), g_protection.trampoline_bytes.size())) {
        return false;
    }
    g_protection.original = reinterpret_cast<NtProtectMemory>(trampoline);
    safetyhook::set_protection_override(protect_memory);
    SPDLOG_INFO("[HiFiRush][HookMemory] Validated game-owned protection detour {:x}, original stub {:x}; "
                "checked hook writes enabled only for Hi-Fi RUSH UE4.27", detour, trampoline);
    return true;
}
}

std::optional<uintptr_t> find_version_marker(uintptr_t image_base) {
    return find_hbk_version_marker(image_base);
}

bool initialize_hook_memory(bool (*is_ue427)()) {
    std::array<wchar_t, 32768> filename{};
    const auto length = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
    if (!length || length >= filename.size() || !matches_game(std::wstring_view{filename.data(), length}, true)) {
        return true;
    }
    static const auto hbk_marker = find_version_marker(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)));
    if (!hbk_marker && (!is_ue427 || !is_ue427())) {
        SPDLOG_ERROR_ONCE("[HiFiRush][HookMemory] No validated UE4.27 version evidence; refusing game-code hook installation");
        return false;
    }
    if (hbk_marker) {
        SPDLOG_INFO_ONCE("[HiFiRush][HookMemory] Validated UE4.27 HBK branch marker at {:x}; retail file version is not used", *hbk_marker);
    }
    static const bool ready = resolve_protection();
    if (!ready) {
        // Still prevent later game-code hooks from entering the unchecked legacy write path.
        safetyhook::set_protection_override(protect_memory);
        SPDLOG_ERROR_ONCE("[HiFiRush][HookMemory] Unsupported protection entry; refusing unsafe game-code hook installation");
    }
    return ready;
}
}
