#pragma once

#include <vector>
#include <windows.h>
#include <psapi.h> // EnumProcessModules, GetModuleInformation

#pragma comment(lib, "Psapi.lib")

struct ModuleRange {
    uintptr_t start;
    uintptr_t end;
};

inline std::vector<ModuleRange> g_moduleRanges;

inline void RefreshModuleRanges()
{
    HMODULE hMods[1024];
    DWORD cbNeeded = 0;

    HANDLE hProcess = GetCurrentProcess();
    if (!EnumProcessModules(hProcess, hMods, sizeof(hMods), &cbNeeded)) {
        return;
    }

    const DWORD count = cbNeeded / sizeof(HMODULE);
    g_moduleRanges.clear();
    g_moduleRanges.reserve(count);

    for (DWORD i = 0; i < count; ++i) {
        MODULEINFO mi{};
        if (GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi))) {
            auto base = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
            auto end  = base + mi.SizeOfImage;
            g_moduleRanges.push_back({ base, end });
        }
    }
}

inline bool IsInAnyModule(const void* ptr)
{
    if (!ptr) return false;

    const auto addr = reinterpret_cast<uintptr_t>(ptr);

    for (const auto& m : g_moduleRanges) {
        if (addr >= m.start && addr < m.end) {
            return true;
        }
    }

    return false;
}

inline bool IsExecutableAddress(const void* ptr)
{
    if (!ptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ptr, &mbi, sizeof(mbi))) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) {
        return false;
    }

    const DWORD prot = mbi.Protect & 0xff; // mask out PAGE_GUARD, etc.

    switch (prot) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool IsValidFunctionPointer(const void* ptr)
{
    if (!ptr) return false;

    // 1) Must be within some module image
    if (!IsInAnyModule(ptr)) {
        return false;
    }

    // 2) That page must be executable
    if (!IsExecutableAddress(ptr)) {
        return false;
    }

    return true;
}
