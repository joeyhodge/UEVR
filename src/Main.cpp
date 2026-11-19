// dllmain
#include <windows.h>
#include <cstdint>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <utility/Thread.hpp>

#include "Framework.hpp"

void startup_thread(HMODULE poc_module) {
    g_framework = std::make_unique<Framework>(poc_module);
}

BOOL APIENTRY DllMain(HANDLE handle, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)startup_thread, handle, 0, nullptr);
    }

    // The game is exiting (reserved != nullptr: ExitProcess, not FreeLibrary). Its other threads are already gone,
    // so tearing the framework down from the CRT's static destructors releases D3D objects into a driver that
    // waits on those threads forever (TEKKEN 8 Demo stayed in ~TextureContext -> dxgi -> Intel driver after quitting).
    // The OS reclaims everything anyway: leave the framework alone.
    if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        (void)g_framework.release();
    }

    return TRUE;
}