#define NOMINMAX
#include <Windows.h>
#include <safetyhook.hpp>
#include <safetyhook/os.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <thread>
#include "utility/HiFiRushHookMemory.hpp"
#include "utility/ProtectedHookTrampoline.hpp"

namespace {
int failures{};
void* blocked{};
bool reject_all{};
bool version_probe_called{};
bool version_probe() { version_probe_called = true; return false; }
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool checked_protect(uint8_t* ptr, size_t size, uint32_t flags, uint32_t* old) {
    if (reject_all || ptr == blocked) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    DWORD previous{};
    if (!VirtualProtect(ptr, size, flags, &previous)) { return false; }
    *old = previous;
    return true;
}
DWORD protection(void* p) {
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(p, &info, sizeof(info));
    return info.Protect;
}
bool rejected_write(uint8_t* p) {
    __try { *p = 0x90; return false; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}
__declspec(noinline) int replacement() { return 11; }
void mid_replacement(safetyhook::Context&) {}
}

int wmain(int argc, wchar_t** argv) {
    using namespace uevr::hifi;
    expect(initialize_hook_memory(version_probe), "other executable retains normal initialization");
    expect(!version_probe_called && !safetyhook::has_protection_override(), "other executable does not probe or opt in");
    expect(matches_game(L"C:\\Games\\Hi-Fi-RUSH.exe", true), "exact game and UE4.27 opt in");
    expect(matches_game(L"hi-fi-rush.exe", true), "case-independent basename");
    expect(!matches_game(L"Hi-Fi-RUSH.exe", false), "other engine versions stay unchanged");
    for (auto name : {L"DaysGone.exe", L"HellbladeGame-Win64-Shipping.exe", L"SHCO.exe",
                     L"Hi-Fi-RUSH.exe.bak", L"Hi-Fi-RUSH-other.exe"}) {
        expect(!matches_game(name, true), "other games and suffixes do not opt in");
    }
    const auto bytes_of = [](std::u16string_view value) {
        return std::span{reinterpret_cast<const uint8_t*>(value.data()), (value.size() + 1) * sizeof(char16_t)};
    };
    expect(find_hbk_ue427_branch(bytes_of(u"++ue+ue_main+4.27hbk")).has_value(),
        "actual HBK engine branch passes despite retail file version 0.3743.0.7060");
    for (const auto branch : {u"++ue+ue_main+4.26hbk", u"++ue+ue_main+5.27hbk", u"++ue+ue_main+4.27hbk_extra"}) {
        expect(!find_hbk_ue427_branch(bytes_of(branch)), "other branches and suffixes do not pass");
    }
    const auto branch_bytes = bytes_of(u"++ue+ue_main+4.27hbk");
    expect(!find_hbk_ue427_branch(branch_bytes.first(branch_bytes.size() - 2)), "unterminated marker is rejected");
    expect(!find_version_marker(0), "invalid image is rejected without dereferencing it");
    if (argc == 3 && std::wstring_view{argv[1]} == L"--image") {
        const auto file = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        const auto mapping = file == INVALID_HANDLE_VALUE ? nullptr :
            CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE_NO_EXECUTE, 0, 0, nullptr);
        const auto view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
        expect(view != nullptr, "captured image maps read-only without executing it");
        if (view) {
            const auto marker = find_version_marker(reinterpret_cast<uintptr_t>(view));
            expect(marker.has_value(), "production scanner finds the captured game's real branch marker");
            if (marker) { std::cout << "Captured HBK marker RVA: 0x" << std::hex << *marker - reinterpret_cast<uintptr_t>(view) << std::dec << '\n'; }
            UnmapViewOfFile(view);
        }
        if (mapping) { CloseHandle(mapping); }
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); }
    }
    const std::array<uint8_t, 8> original{0x4c, 0x8b, 0xd1, 0xb8, 0x50, 0, 0, 0};
    std::array<uint8_t, 13> stub{};
    std::memcpy(stub.data(), original.data(), original.size());
    stub[8] = 0xe9;
    constexpr uintptr_t entry = 0x7ffb00010000;
    for (const auto candidate : {entry - 0x10000, entry + 0x180000}) {
        const auto disp = static_cast<int32_t>(entry + 8 - (candidate + stub.size()));
        std::memcpy(stub.data() + 9, &disp, sizeof(disp));
        expect(matches_original_stub(stub, candidate, original, entry), "both signs of relative branch match");
        stub[4] ^= 1;
        expect(!matches_original_stub(stub, candidate, original, entry), "wrong service bytes rejected");
        stub[4] ^= 1;
        stub[9] ^= 1;
        expect(!matches_original_stub(stub, candidate, original, entry), "wrong continuation rejected");
        stub[9] ^= 1;
        expect(!matches_original_stub(std::span{stub}.first(12), candidate, original, entry), "truncation rejected");
    }

    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    auto* second = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page || !second) { return 2; }
    std::memset(page, 0x90, 32);
    const uint8_t function[]{0xb8, 7, 0, 0, 0, 0xc3};
    std::memcpy(page, function, sizeof(function));
    DWORD old{};
    VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    VirtualProtect(second, 0x1000, PAGE_EXECUTE_READ, &old);
    safetyhook::set_protection_override(checked_protect);

    bool wrote{};
    blocked = page;
    safetyhook::trap_threads(page, second, 8, [&] { wrote = true; });
    expect(!wrote && protection(page) == PAGE_EXECUTE_READ, "first protection failure performs no write");
    expect(rejected_write(page), "write faults propagate instead of being retried forever");
    blocked = second;
    safetyhook::trap_threads(page, second, 8, [&] { wrote = true; });
    expect(!wrote && protection(page) == PAGE_EXECUTE_READ, "second failure restores first protection");

    blocked = nullptr;
    safetyhook::trap_threads(page, second, 8, [&] { wrote = true; });
    expect(wrote && protection(page) == PAGE_EXECUTE_READ, "successful trap restores executable pages");
    expect(rejected_write(page), "write AV is not swallowed even with a retained trap registration");
    auto result = safetyhook::InlineHook::create(page, reinterpret_cast<void*>(&replacement),
        safetyhook::InlineHook::StartDisabled);
    expect(result.has_value(), "test inline hook relocates");
    if (result) {
        auto& hook = *result;
        blocked = page;
        expect(!hook.enable(), "failed protection is returned to the installer");
        expect(std::memcmp(page, function, sizeof(function)) == 0, "failed installation preserves original bytes");
        blocked = nullptr;
        expect(hook.enable().has_value(), "validated write installs hook");
        expect(reinterpret_cast<int(*)()>(page)() == 11, "installed hook executes replacement");
        expect(hook.call<int>() == 7, "original trampoline remains callable");
        reject_all = true;
        expect(!hook.disable(), "failed uninstall reports failure");
        reject_all = false;
        expect(reinterpret_cast<int(*)()>(page)() == 11, "failed uninstall preserves active hook");
        expect(hook.disable().has_value(), "valid uninstall succeeds");
        expect(reinterpret_cast<int(*)()>(page)() == 7, "uninstall restores original behavior");

        std::atomic_bool stop{false};
        std::atomic_uint invalid_results{0};
        std::atomic_uint calls{0};
        std::thread reader{[&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const auto value = reinterpret_cast<int(*)()>(page)();
                if (value != 7 && value != 11) { ++invalid_results; }
                ++calls;
            }
        }};
        while (calls.load() == 0) { std::this_thread::yield(); }
        for (int i = 0; i < 100; ++i) {
            expect(hook.enable().has_value(), "concurrent opt-in installation succeeds");
            expect(hook.disable().has_value(), "concurrent opt-in removal succeeds");
        }
        stop.store(true, std::memory_order_relaxed);
        reader.join();
        expect(invalid_results.load() == 0, "execution traps preserve concurrent calls");
        safetyhook::set_protection_override(nullptr);
        expect(hook.enable().has_value() && reinterpret_cast<int(*)()>(page)() == 11,
            "default non-opt-in hook path still works");
        expect(hook.disable().has_value(), "default uninstall still works");
    }

    safetyhook::set_protection_override(checked_protect);
    auto mid = safetyhook::MidHook::create(page, mid_replacement);
    expect(mid.has_value(), "opt-in midhook installs");
    if (mid) {
        expect(reinterpret_cast<int(*)()>(page)() == 7, "midhook preserves original result");
        blocked = page;
        mid->reset();
        expect(!*mid, "failed reset releases the caller's midhook handle");
        blocked = nullptr;
        expect(reinterpret_cast<int(*)()>(page)() == 7, "failed reset retains both executable stubs");
        // The process-lifetime retention is intentional; restore this test-owned target before freeing it.
        VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
        std::memcpy(page, function, sizeof(function));
        VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old);
    }
    safetyhook::set_protection_override(nullptr);
    VirtualFree(page, 0, MEM_RELEASE);
    VirtualFree(second, 0, MEM_RELEASE);
    std::cout << "Hi-Fi hook-memory tests: " << failures << " failures\n";
    return failures ? 1 : 0;
}
