#define NOMINMAX
#include <array>
#include <cstring>
#include <iostream>
#include <sdk/CVar.hpp>
#include <sdk/MafiaDiscovery.hpp>
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

    // Manager recovery must validate memory without invoking any candidate vfunc.
    static std::array<void*, 8> manager_vtable;
    static std::array<void*, 3> variable_vtable;
    manager_vtable.fill(reinterpret_cast<void*>(&destructor_stub));
    variable_vtable.fill(reinterpret_cast<void*>(&destructor_stub));
    struct Variable { void** vtable; } variables[3]{{variable_vtable.data()}, {variable_vtable.data()}, {variable_vtable.data()}};
    std::array<sdk::ConsoleObjectElement, 3> elements{};
    constexpr std::array<std::wstring_view, 3> names{L"r.DumpingMovie", L"r.DetailMode", L"r.OneFrameThreadLag"};
    for (size_t i = 0; i < names.size(); ++i) {
        elements[i].key = const_cast<wchar_t*>(names[i].data());
        elements[i].unk[0] = elements[i].unk[1] = static_cast<int32_t>(names[i].size() + 1);
        elements[i].value = reinterpret_cast<sdk::IConsoleObject*>(&variables[i]);
    }
    struct Manager { void** vtable; sdk::ConsoleObjectArray array; } manager{manager_vtable.data(), {elements.data(), 3, 3}};
    static_assert(offsetof(Manager, array) == sizeof(void*));
    const auto valid_manager = [&] {
        return sdk::mafia::detail::has_valid_console_manager_map(reinterpret_cast<sdk::FConsoleManager*>(&manager));
    };
    expect(valid_manager(), "initialized stock console map requires three independently validated CVar anchors");
    expect(!sdk::mafia::detail::has_valid_console_manager_map(nullptr), "null manager is rejected without a call");
    expect(!sdk::mafia::detail::has_valid_console_manager_map(reinterpret_cast<sdk::FConsoleManager*>(0x10000)),
        "unreadable manager fails closed without an access violation");
    manager.array.capacity = 2;
    expect(!valid_manager(), "invalid console map capacity is rejected");
    manager.array.capacity = 3;
    manager.array.elements = reinterpret_cast<sdk::ConsoleObjectElement*>(0x10000);
    expect(!valid_manager(), "unreadable console elements fail closed");
    manager.array.elements = elements.data();
    elements[2].value = elements[1].value;
    expect(!valid_manager(), "aliased anchor objects cannot validate a manager");
    elements[2].value = reinterpret_cast<sdk::IConsoleObject*>(&variables[2]);
    elements[2].key = reinterpret_cast<wchar_t*>(0x10000);
    expect(!valid_manager(), "unreadable anchor names are rejected without a call");
    elements[2].key = const_cast<wchar_t*>(names[2].data());
    elements[2].unk[0] = 0x7fffffff;
    expect(!valid_manager(), "unbounded string lengths cannot be read during discovery");
    elements[2].unk[0] = elements[2].unk[1];
    variable_vtable[1] = reinterpret_cast<void*>(0x10000);
    expect(!valid_manager(), "non-executable console virtual functions prevent publication");
    variable_vtable[1] = reinterpret_cast<void*>(&destructor_stub);
    expect(valid_manager(), "a later valid snapshot is accepted after earlier incomplete snapshots");
    return failures;
}
