#include <iostream>
#include <string>

#include <imgui_internal.h>
#include "utility/ImGui.hpp"

int test_console_text() {
    int failures{};
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
