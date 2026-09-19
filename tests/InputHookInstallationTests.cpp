#define NOMINMAX
#include <Windows.h>

#include <safetyhook/os.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "utility/InputHookInstallation.hpp"

namespace {
using namespace std::chrono_literals;
using uevr::input_hooks::acquire_shf_installation_lock;
using uevr::input_hooks::requires_shf_installation_lock;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

void wait_ready(std::future<void>& ready) {
    require(ready.wait_for(5s) == std::future_status::ready, "bounded hook-installation handshake");
    ready.get();
}

class Page {
public:
    Page() : data{static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))} {
        require(data != nullptr, "allocate isolated fixture page");
    }
    ~Page() { VirtualFree(data, 0, MEM_RELEASE); }
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;
    uint8_t* data;
};

DWORD protection(void* address) {
    MEMORY_BASIC_INFORMATION info{};
    require(VirtualQuery(address, &info, sizeof(info)) == sizeof(info), "query fixture page protection");
    return info.Protect;
}

void test_executable_guard() {
    for (const auto path : {
             L"SHf-Win64-Shipping.exe", L"shf-win64-shipping.exe",
             L"D:\\Steam\\SILENT HILL f\\SHf\\Binaries\\Win64\\SHf-Win64-Shipping.exe",
             L"D:/Games/SHf-Win64-SHIPPING.EXE", L"C:\\Games\\\u65e5\\SHf-Win64-Shipping.exe"}) {
        require(requires_shf_installation_lock(path), "exact case-independent SHf basename opts in");
    }
    const std::wstring long_path = L"C:\\" + std::wstring(512, L'a') + L"\\SHf-Win64-Shipping.exe";
    require(requires_shf_installation_lock(long_path), "long complete executable path opts in");

    for (const auto path : {
             L"", L"C:\\Games\\SHf-Win64-Shipping.exe\\", L"SHf-Win64-Shipping.exe.bak",
             L"Other-SHf-Win64-Shipping.exe", L"SHf-Win64-Shipping.exe ", L"SHf-WinGDK-Shipping.exe",
             L"C:\\SHf-Win64-Shipping.exe\\Other.exe", L"SilentHill2-Win64-Shipping.exe",
             L"Hi-Fi-RUSH.exe", L"NASCAR26_Steam-Win64-Shipping.exe", L"prospi-Win64-Shipping.exe"}) {
        require(!requires_shf_installation_lock(path), "other or incomplete executable names remain unchanged");
    }
    auto embedded_null = std::wstring{L"C:\\Games"};
    embedded_null.push_back(L'\0');
    embedded_null += L"\\SHf-Win64-Shipping.exe";
    require(!requires_shf_installation_lock(embedded_null), "embedded NUL does not identify an executable");
}

void test_lock_scope() {
    std::recursive_mutex monitor;
    std::unique_lock held{monitor};
    std::promise<void> other_done;
    auto other_ready = other_done.get_future();
    std::thread other([&] {
        auto lock = acquire_shf_installation_lock(L"Other.exe", monitor);
        require(!lock.owns_lock(), "other games do not acquire even a contended mutex");
        other_done.set_value();
    });
    wait_ready(other_ready);
    other.join();
    {
        auto nested = acquire_shf_installation_lock(L"SHf-Win64-Shipping.exe", monitor);
        require(nested.owns_lock(), "existing recursive monitor ownership remains supported");
    }
    held.unlock();
    try {
        auto lock = acquire_shf_installation_lock(L"SHf-Win64-Shipping.exe", monitor);
        require(lock.owns_lock(), "SHf acquires the existing monitor");
        throw 1;
    } catch (int) {
    }
    std::thread after_failure([&] {
        require(monitor.try_lock(), "guard releases the mutex on an installation failure");
        monitor.unlock();
    });
    after_failure.join();
}

void test_shared_trampoline_page() {
    require(!safetyhook::has_protection_override(), "fixture uses the normal protection path");
    Page first, second, shared;
    std::recursive_mutex monitor;
    int unguarded_nx{};
    constexpr int iterations = 30;

    for (const bool guarded : {false, true}) {
        for (int iteration = 0; iteration < iterations; ++iteration) {
            DWORD old{};
            require(VirtualProtect(shared.data, 4096, PAGE_EXECUTE_READWRITE, &old) != FALSE,
                "reset shared fixture page protection");
            std::promise<void> a_entered, b_entered, a_done;
            auto a_ready = a_entered.get_future();
            auto b_ready = b_entered.get_future();
            auto a_finished = a_done.get_future();
            std::atomic<bool> a_restored{};

            // XInput already holds the monitor; exercise DirectInput's production
            // guard against two restores to different trampolines on the same page.
            std::thread a([&] {
                {
                    std::scoped_lock lock{monitor};
                    safetyhook::trap_threads(first.data, shared.data, 5, [&] {
                        a_entered.set_value();
                        wait_ready(b_ready);
                    });
                    a_restored.store(true, std::memory_order_release);
                }
                a_done.set_value();
            });
            std::thread b([&] {
                wait_ready(a_ready);
                if (guarded) {
                    b_entered.set_value();
                }
                auto lock = acquire_shf_installation_lock(
                    guarded ? L"SHf-Win64-Shipping.exe" : L"Other.exe", monitor);
                require(lock.owns_lock() == guarded, "only SHf serializes the installation");
                safetyhook::trap_threads(second.data, shared.data + 32, 5, [&] {
                    if (guarded) {
                        require(a_restored.load(std::memory_order_acquire),
                            "second SHf protection change follows the first restore");
                    } else {
                        b_entered.set_value();
                        wait_ready(a_finished);
                    }
                });
            });
            a.join();
            b.join();

            const auto actual = protection(shared.data);
            if (guarded) {
                require(actual == PAGE_EXECUTE_READWRITE, "SHf trampoline page remains executable");
            } else {
                // Keep the control informative if a later SafetyHook independently fixes this race.
                require(actual == PAGE_READWRITE || actual == PAGE_EXECUTE_READWRITE,
                    "control protection has a recognized result");
                unguarded_nx += actual == PAGE_READWRITE;
            }
            require(protection(first.data) == PAGE_EXECUTE_READWRITE &&
                    protection(second.data) == PAGE_EXECUTE_READWRITE,
                "source page permissions are restored");
            require(!safetyhook::has_protection_override(), "no global protection override is introduced");
        }
    }
    std::cout << "Unguarded NX observations: " << unguarded_nx << '/' << iterations
              << "; guarded executable restores: " << iterations << '/' << iterations << '\n';
}
} // namespace

int main() {
    test_executable_guard();
    test_lock_scope();
    test_shared_trampoline_page();
    std::cout << "Input hook installation tests passed\n";
    return 0;
}
