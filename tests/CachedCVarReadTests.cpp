#define NOMINMAX
#include <array>
#include <cstring>
#include <iostream>
#include <sdk/CVar.hpp>
#include <utility/Scan.hpp>

namespace {
struct CacheAccess : sdk::IConsoleVariable {
    static void seed(void* vtable) {
        std::scoped_lock lock{s_vtable_mutex};
        VtableInfo info{};
        info.get_int_vtable_index = 1;
        info.get_float_vtable_index = 2;
        s_vtable_infos[vtable] = info;
    }
    static void erase(void* vtable) {
        std::scoped_lock lock{s_vtable_mutex};
        s_vtable_infos.erase(vtable);
    }
};
int calls{};
void destructor_stub() {}
int32_t integer_getter(sdk::IConsoleVariable*) { ++calls; return 0; }
float float_getter(sdk::IConsoleVariable*) { ++calls; return 0.5f; }
int32_t faulting_getter(sdk::IConsoleVariable*) {
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    return 0;
}
}

int test_cached_cvar_reads() {
    int failures{};
    const auto expect = [&](bool value, const char* message) {
        if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    std::array<void*, 3> vtable{reinterpret_cast<void*>(&destructor_stub),
        reinterpret_cast<void*>(&integer_getter), reinterpret_cast<void*>(&float_getter)};
    struct Object { void** vtable; } object{vtable.data()};
    auto* variable = reinterpret_cast<sdk::IConsoleVariable*>(&object);
    expect(!variable->TryGetInt() && calls == 0, "diagnostic getter never discovers an unknown vtable");
    CacheAccess::seed(vtable.data());
    const auto zero = variable->TryGetInt();
    expect(zero && *zero == 0 && calls == 1, "cached getter preserves a genuine zero");
    expect(variable->TryGetFloat() == 0.5f && calls == 2, "cached float getter is callable");
    vtable[1] = reinterpret_cast<void*>(&faulting_getter);
    expect(!variable->TryGetInt(), "structured getter exception reports unavailable, not zero");
    CacheAccess::erase(vtable.data());
    expect(!variable->TryGetFloat() && calls == 2, "cleared cache is not rediscovered by diagnostics");
    expect(sdk::find_validated_cvar_cached_only(L"uevr_diagnostic_nonexistent") == nullptr,
        "cache-only lookup never creates or discovers a missing CVar");

    // The standalone SDK dependency only has the one-argument decoder API.
    std::array<uint8_t, 32> lea{0x48, 0x8D, 0x15};
    const auto instruction = reinterpret_cast<uintptr_t>(lea.data());
    for (const int32_t displacement : {-64, 0, 64}) {
        std::memcpy(lea.data() + 3, &displacement, sizeof(displacement));
        const auto resolved = utility::resolve_displacement(instruction);
        const auto expected = static_cast<uintptr_t>(static_cast<intptr_t>(instruction) + 7 + displacement);
        expect(resolved && *resolved == expected, "portable CVar LEA decoding preserves signed RIP-relative targets");
    }
    return failures;
}
