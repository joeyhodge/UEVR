#include "UE57SlateSymbols.hpp"

#include <windows.h>
#include <DbgHelp.h>
#include <spdlog/spdlog.h>

#include <mutex>
#include <array>
#include <optional>

namespace vrmod {
namespace {
constexpr char ADD_SLATE_DRAW_ELEMENTS_PASS_NAME[] =
    "?AddSlateDrawElementsPass@@YAXAEAVFRDGBuilder@@AEBVFSlateRHIRenderingPolicy@@AEBUFSlateDrawElementsPassInputs@@V?$TArrayView@$$CBVFSlateRenderBatch@@H@@H@Z";
constexpr char REGISTER_EXTERNAL_TEXTURE_FROM_RHI_NAME[] =
    "?RegisterExternalTexture@@YAPEAVFRDGTexture@@AEAVFRDGBuilder@@PEAVFRHITexture@@PEB_W@Z";

std::optional<uintptr_t> resolve_symbol(HANDLE process, const char* name) {
    std::array<uint8_t, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> storage{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    if (!SymFromName(process, name, symbol)) {
        SPDLOG_INFO("Optional UE 5.7 Slate symbol was not resolved: {}", name);
        return std::nullopt;
    }

    return static_cast<uintptr_t>(symbol->Address);
}

UE57SlateSymbols resolve() {
    UE57SlateSymbols symbols{};
    const auto process = GetCurrentProcess();

    static std::once_flag init_once{};
    static bool initialized{false};

    std::call_once(init_once, [&]() {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        initialized = SymInitialize(process, nullptr, TRUE) == TRUE;

        if (!initialized) {
            SPDLOG_ERROR("Failed to initialize DbgHelp for UE 5.7 Slate symbol resolution");
            return;
        }

        wchar_t exe_path[MAX_PATH]{};
        const auto path_len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        const auto base = reinterpret_cast<DWORD64>(GetModuleHandleW(nullptr));

        if (path_len != 0 && base != 0) {
            SymLoadModuleExW(process, nullptr, exe_path, nullptr, base, 0, nullptr, 0);
        }
    });

    if (!initialized) {
        return symbols;
    }

    if (const auto addr = resolve_symbol(process, ADD_SLATE_DRAW_ELEMENTS_PASS_NAME)) {
        symbols.add_slate_draw_elements_pass = *addr;
    }

    if (const auto addr = resolve_symbol(process, REGISTER_EXTERNAL_TEXTURE_FROM_RHI_NAME)) {
        symbols.register_external_texture_from_rhi = *addr;
    }

    if (symbols.valid()) {
        SPDLOG_INFO(
            "Resolved UE 5.7 Slate symbols: AddSlateDrawElementsPass={:x}, RegisterExternalTexture(FRHITexture*)={:x}",
            symbols.add_slate_draw_elements_pass,
            symbols.register_external_texture_from_rhi);
    } else {
        SPDLOG_INFO(
            "Incomplete optional UE 5.7 Slate symbol resolution: AddSlateDrawElementsPass={:x}, RegisterExternalTexture(FRHITexture*)={:x}",
            symbols.add_slate_draw_elements_pass,
            symbols.register_external_texture_from_rhi);
    }

    return symbols;
}
}

const UE57SlateSymbols& get_ue57_slate_symbols() {
    static UE57SlateSymbols symbols = resolve();
    return symbols;
}
}
