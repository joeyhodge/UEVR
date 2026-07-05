#include <Windows.h>

#include <iostream>

#include <sdk/SafeUObjectAccess.hpp>

int main() {
    constexpr size_t page_size = 4096;
    auto* pages = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, page_size * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (pages == nullptr) {
        return 1;
    }

    *reinterpret_cast<uint32_t*>(pages) = 0x12345678;
    DWORD old_protection{};
    VirtualProtect(pages + page_size, page_size, PAGE_NOACCESS, &old_protection);

    uint32_t value{};
    if (!sdk::safe_uobject::try_read(pages, value) || value != 0x12345678) {
        return 2;
    }
    if (sdk::safe_uobject::try_read(pages + page_size, value)) {
        return 3;
    }

    sdk::safe_uobject::set_mode(sdk::UObjectAccessMode::StrictDiagnostics);
    if (sdk::safe_uobject::get_mode() != sdk::UObjectAccessMode::StrictDiagnostics) {
        return 4;
    }
    if (sdk::safe_uobject::acquire_identity(nullptr).error != sdk::UObjectAccessError::NullPointer) {
        return 5;
    }

    const auto diagnostics = sdk::safe_uobject::get_diagnostics();
    if (diagnostics.safe_read_failures == 0 || diagnostics.identity_failures == 0) {
        return 6;
    }

    VirtualFree(pages, 0, MEM_RELEASE);
    std::cout << "safe UObject access test passed\n";
    return 0;
}
