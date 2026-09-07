#define NOMINMAX
#include <array>
#include <iostream>
#include <sdk/CVar.hpp>

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
    return failures;
}
