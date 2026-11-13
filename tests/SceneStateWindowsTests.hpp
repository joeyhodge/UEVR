#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <array>
#include <cstring>
#include "utility/SceneStateValidation.hpp"

namespace scene_state_windows_tests {
template<class Expect> void run(const Expect& expect) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    if (system.dwPageSize != 4096) { expect(false, "owned-page fixture requires 4 KiB Windows pages"); return; }
    constexpr size_t page = 4096;
    alignas(page) static std::array<uintptr_t, page / sizeof(uintptr_t)> image_table{};
    auto* states = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    auto* code = static_cast<uint8_t*>(VirtualAlloc(nullptr, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!states || !code) {
        expect(false, "Windows safety fixture allocation succeeds");
        if (states) { VirtualFree(states, 0, MEM_RELEASE); }
        if (code) { VirtualFree(code, 0, MEM_RELEASE); }
        return;
    }
    const auto left = reinterpret_cast<uintptr_t>(states), right = left + page;
    const auto table = reinterpret_cast<uintptr_t>(image_table.data());
    std::memcpy(states, &table, sizeof(table));
    std::memcpy(states + page, &table, sizeof(table));
    image_table[0] = reinterpret_cast<uintptr_t>(code);
    const auto range = [](uintptr_t address, size_t size, bool executable) {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !size || size > UINTPTR_MAX - address ||
            !VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
        const auto base = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (address < base || address - base > info.RegionSize || size > info.RegionSize - (address - base)) { return false; }
        const auto p = info.Protect & 0xff;
        return executable ? p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY
                          : p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY ||
                            p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    };
    const auto make = [&] {
        return utility::scene_state_validation::batch<2>(
            [&](uintptr_t address, uintptr_t& out) {
                if (!range(address, sizeof(out), false)) { return false; }
                std::memcpy(&out, reinterpret_cast<void*>(address), sizeof(out));
                return true;
            },
            [&](uintptr_t vtable, uintptr_t first) {
                HMODULE module{};
                return range(first, 1, true) && GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(vtable), &module) != FALSE;
            });
    };
    const auto pair = [&] { auto checks = make(); return checks.check(left) && checks.check(right); };
    DWORD previous{};
    const auto protect = [&](void* address, DWORD flags) {
        const auto ok = VirtualProtect(address, page, flags, &previous) != FALSE;
        expect(ok, "owned-page protection changes succeed");
        return ok;
    };
    expect(!pair(), "readable but non-executable code is rejected");
    if (protect(code, PAGE_EXECUTE_READ)) {
        expect(pair(), "two live states can share an image-owned type with executable code");
        if (protect(code, PAGE_READWRITE)) { expect(!pair(), "a new operation sees revoked execution permission"); }
        protect(code, PAGE_EXECUTE_READ);
    }
    for (const DWORD flags : {DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_NOACCESS)}) {
        auto checks = make();
        expect(checks.check(left), "first eye establishes the local type proof");
        if (protect(image_table.data(), flags)) {
            expect(!checks.check(right), "even a common table is read freshly after protection changes");
            MEMORY_BASIC_INFORMATION info{};
            const auto queried = VirtualQuery(image_table.data(), &info, sizeof(info));
            expect(queried && (!(flags & PAGE_GUARD) || (info.Protect & PAGE_GUARD)),
                "rejecting a guarded slot does not consume the guard via a second read");
            protect(image_table.data(), PAGE_READWRITE);
        }
    }
    for (const DWORD flags : {DWORD(PAGE_READWRITE | PAGE_GUARD), DWORD(PAGE_NOACCESS)}) {
        if (protect(states + page, flags)) {
            expect(!pair(), "guard/noaccess on the other eye's state cannot reuse the common table proof");
            protect(states + page, PAGE_READWRITE);
        }
    }
    if (protect(code, PAGE_EXECUTE_READ | PAGE_GUARD)) {
        expect(!pair(), "guarded executable code is not callable");
        MEMORY_BASIC_INFORMATION info{};
        expect(VirtualQuery(code, &info, sizeof(info)) && (info.Protect & PAGE_GUARD), "code guard is not consumed");
        protect(code, PAGE_EXECUTE_READ);
    }
    {
        auto checks = make();
        expect(checks.check(left), "live type is initially accepted");
        image_table[0] = right;
        expect(!checks.check(right), "a slot rewritten to non-executable state memory requires fresh proof");
        image_table[0] = reinterpret_cast<uintptr_t>(code);
    }
    const auto private_table = right + sizeof(uintptr_t);
    std::memcpy(states + page + sizeof(uintptr_t), &image_table[0], sizeof(uintptr_t));
    std::memcpy(states + page, &private_table, sizeof(private_table));
    expect(!pair(), "an unowned private vtable does not borrow module ownership from the first eye");
    VirtualFree(states, 0, MEM_RELEASE);
    expect(!make().check(left), "released state pages remain rejected");
    VirtualFree(code, 0, MEM_RELEASE);
    image_table[0] = 0;
}
}
#else
namespace scene_state_windows_tests { template<class Expect> void run(const Expect&) {} }
#endif
