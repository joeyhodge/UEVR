#include <iostream>
#include <string>
#include <array>
#include <cstring>

#include <sdk/ConsoleManager.hpp>
#include <sdk/FName.hpp>
#include <sdk/MafiaDiscovery.hpp>

#include <imgui_internal.h>
#include "utility/ImGui.hpp"

namespace {
int enum_probe_tests() {
    int failures{};
    const auto expect = [&](bool result, const char* message) {
        if (!result) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    const auto previous = sdk::FName::s_is_case_preserving.load();
    for (const bool case_preserving : {false, true}) {
        sdk::FName::s_is_case_preserving.store(case_preserving);
        struct GuardedName { uint32_t before{0x11223344}; sdk::FNameCasePreserving name{}; uint32_t after{0x55667788}; } probe;
        expect(sdk::FName::runtime_size() == (case_preserving ? 12 : 8), "enum guard retains the current borrowed name ABI");
        for (const auto words : {std::array<int32_t, 2>{0, 0}, {37, 29}, {0x20010001, 0}, {-1, 0}}) {
            probe.name.a1 = words[0];
            probe.name.a2 = words[1];
            std::array<uint8_t, sizeof(probe)> before{};
            std::memcpy(before.data(), &probe, sizeof(probe));
            expect(probe.name.to_string_no_numbers() == L"None",
                "None, numbered/mismatched and out-of-range speculative enum names are rejected without engine conversion");
            expect(std::memcmp(before.data(), &probe, sizeof(probe)) == 0,
                "enum candidate checks cannot overwrite borrowed name storage or adjacent canaries");
        }
    }
    sdk::FName::s_is_case_preserving.store(previous);
    return failures;
}

size_t console_validation_calls{};
void console_validation_must_not_call() { ++console_validation_calls; }

int console_map_validation_tests() {
    int failures{};
    const auto expect = [&](bool result, const char* message) {
        if (!result) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
    };
    static const std::array<uintptr_t, 8> vtable{
        reinterpret_cast<uintptr_t>(&console_validation_must_not_call), reinterpret_cast<uintptr_t>(&console_validation_must_not_call),
        reinterpret_cast<uintptr_t>(&console_validation_must_not_call), reinterpret_cast<uintptr_t>(&console_validation_must_not_call),
        reinterpret_cast<uintptr_t>(&console_validation_must_not_call), reinterpret_cast<uintptr_t>(&console_validation_must_not_call),
        reinterpret_cast<uintptr_t>(&console_validation_must_not_call), reinterpret_cast<uintptr_t>(&console_validation_must_not_call)};
    struct Object { const uintptr_t* vtable; } objects[3]{{vtable.data()}, {vtable.data()}, {vtable.data()}};
    std::array<sdk::ConsoleObjectElement, 3> entries{};
    constexpr std::array<std::wstring_view, 3> names{L"r.DumpingMovie", L"r.DetailMode", L"r.OneFrameThreadLag"};
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].key = const_cast<wchar_t*>(names[i].data());
        entries[i].unk[0] = entries[i].unk[1] = static_cast<int32_t>(names[i].size() + 1);
        entries[i].value = reinterpret_cast<sdk::IConsoleObject*>(&objects[i]);
    }
    struct Manager { const uintptr_t* vtable; sdk::ConsoleObjectArray table; } manager{vtable.data(), {entries.data(), 3, 3}};
    const auto validate = [](Manager* value) {
        return sdk::mafia::detail::has_valid_console_manager_map(reinterpret_cast<sdk::FConsoleManager*>(value));
    };
    expect(validate(&manager), "actual console validator accepts a coherent map with independent known CVar names and module vtables");
    static Manager static_manager{};
    static_manager = manager;
    expect(validate(&static_manager), "a structurally valid module-resident manager is not rejected by a heap-only assumption");
    static_manager = {};
    manager.table.count = 2097263;
    expect(!validate(&manager), "the reported Stalker bogus console count cannot pass table validation");
    manager.table.count = 3;
    manager.table.capacity = 2;
    expect(!validate(&manager), "console capacity must cover its live elements");
    manager.table.capacity = 3;
    entries[1].value = entries[0].value;
    expect(!validate(&manager), "independent CVar names cannot alias one fake console object");
    entries[1].value = reinterpret_cast<sdk::IConsoleObject*>(&objects[1]);
    const auto saved = entries[2].key;
    entries[2].key = const_cast<wchar_t*>(L"not.a.registered.cvar");
    expect(!validate(&manager), "a merely readable array without all independent anchors is rejected");
    entries[2].key = saved;
    std::array<uintptr_t, 8> non_image_vtable = vtable;
    manager.vtable = non_image_vtable.data();
    expect(!validate(&manager), "a readable table cannot substitute a non-module manager vtable");
    expect(!validate(nullptr), "a null singleton is rejected");
    expect(console_validation_calls == 0, "console-map discovery never calls any candidate virtual function");
    return failures;
}
}

int test_console_text() {
    int failures = enum_probe_tests() + console_map_validation_tests();
    const auto previous = ImGui::GetCurrentContext();
    const auto context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(800, 600);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(700, 500));
    ImGui::Begin("Console text regression");
    ImGui::LogToBuffer();
    const std::string help = "Literal help: %s %ls %n %08x 100% %%";
    imgui::text_wrapped_unformatted(help);
    if (std::string_view{context->LogBuffer.c_str()}.find(help) == std::string_view::npos) {
        ++failures;
        std::cerr << "FAILED: console help is rendered literally, including percent directives\n";
    }
    const char bounded[] = {'O', 'K', '%', 's'};
    imgui::text_wrapped_unformatted(std::string_view{bounded, 2});
    imgui::text_wrapped_unformatted({});
    if (context->CurrentWindow->DC.TextWrapPosStack.Size != 0) {
        ++failures;
        std::cerr << "FAILED: console help restores the previous wrap state\n";
    }
    ImGui::LogFinish();
    ImGui::End();
    ImGui::Render();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    return failures;
}
