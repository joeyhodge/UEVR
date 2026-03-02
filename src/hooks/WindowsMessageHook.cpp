#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "utility/Thread.hpp"

#include "WindowsMessageHook.hpp"

using namespace std;

static WindowsMessageHook* g_windows_message_hook{ nullptr };
std::recursive_mutex g_proc_mutex{};

LRESULT WINAPI window_proc(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    std::lock_guard _{ g_proc_mutex };

    if (g_windows_message_hook == nullptr) {
        return DefWindowProc(wnd, message, w_param, l_param);
    }

    auto* const hook = g_windows_message_hook;

    // Call our onMessage callback.
    auto& on_message = hook->on_message;

    if (on_message) {
        // If it returns false we don't call the original window procedure.
        if (!on_message(wnd, message, w_param, l_param)) {
            return DefWindowProc(wnd, message, w_param, l_param);
        }
    }

    const auto original = hook->get_original();
    if (original == nullptr || original == &window_proc) {
        return DefWindowProc(wnd, message, w_param, l_param);
    }

    // Call the original message procedure.
    return CallWindowProc(original, wnd, message, w_param, l_param);
}

WindowsMessageHook::WindowsMessageHook(HWND wnd)
    : m_wnd{ wnd },
    m_original_proc{ nullptr }
{
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Initializing WindowsMessageHook");

    g_windows_message_hook = this;

    // Set it to our "hook" procedure.
    SetLastError(0);
    m_original_proc = (WNDPROC)SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)&window_proc);

    if (m_original_proc == nullptr) {
        const auto err = GetLastError();
        if (err != 0) {
            spdlog::error("[WindowsMessageHook] SetWindowLongPtr failed, gle={}", err);
        }
    }

    spdlog::info("Hooked Windows message handler");
}

WindowsMessageHook::~WindowsMessageHook() {
    std::lock_guard _{ g_proc_mutex };
    spdlog::info("Destroying WindowsMessageHook");
    
    remove();
    g_windows_message_hook = nullptr;
}

bool WindowsMessageHook::remove() {
    // Don't attempt to restore invalid original window procedures.
    if (m_original_proc == nullptr || m_wnd == nullptr) {
        return true;
    }

    // Restore the original window procedure.
    auto current_proc = (WNDPROC)GetWindowLongPtr(m_wnd, GWLP_WNDPROC);

    // lets not try to restore the original window procedure if it's not ours.
    if (current_proc == &window_proc) {
        SetWindowLongPtr(m_wnd, GWLP_WNDPROC, (LONG_PTR)m_original_proc);
    }

    // Invalidate this message hook.
    m_wnd = nullptr;
    m_original_proc = nullptr;

    return true;
}

bool WindowsMessageHook::is_hook_intact() {
    if (!m_wnd) {
        return false;
    }

    return GetWindowLongPtr(m_wnd, GWLP_WNDPROC) == (LONG_PTR)&window_proc;
}
