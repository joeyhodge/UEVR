#include <memory>
#include <mutex>
#include <uevr/Plugin.hpp>
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_dx12.h"
#include "imgui/imgui_impl_win32.h"
#include "rendering/d3d11.hpp"
#include "rendering/d3d12.hpp"

using namespace uevr;

namespace {

enum class AltDPadMethod {
    NONE = 0,
    RIGHT_CLICK_LEFT_STICK,
    LEFT_CLICK_RIGHT_STICK,
};

const char* ALT_DPAD_METHOD_NAMES[] = {
    "Disabled",
    "Right Stick Press + Left Stick",
    "Left Stick Press + Right Stick",
};

class AltDPadPlugin : public uevr::Plugin {
public:
    AltDPadPlugin() = default;

    void on_initialize() override {
        ImGui::CreateContext();
        m_joystick_click = API::VR::get_action_handle("/actions/default/in/JoystickClick");
        API::get()->log_info("AltDPadPlugin initialized");
    }

    void on_present() override {
        std::scoped_lock _{m_imgui_mutex};

        if (!m_initialized) {
            if (!initialize_imgui()) {
                return;
            }
        }

        const auto renderer_data = API::get()->param()->renderer;

        ImGui_ImplWin32_NewFrame();

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            ImGui_ImplDX11_NewFrame();
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            ImGui_ImplDX12_NewFrame();
        }

        ImGui::NewFrame();

        const bool main_menu_open = API::get()->is_drawing_ui();

         // If the user toggled the menu off while the AltDPad window is open,
        // close just the AltDPad window and re-open the main menu so the user
        // can continue to interact with it.
        if (!main_menu_open && m_prev_main_menu_open && m_window_open) {
            m_window_open = false;
            PostMessage(m_wnd, WM_KEYDOWN, VK_INSERT, 0);
            PostMessage(m_wnd, WM_KEYUP, VK_INSERT, 0);
        }

        if (main_menu_open && !m_prev_main_menu_open) {
            m_window_open = true;
        }

        m_prev_main_menu_open = main_menu_open;

        if (main_menu_open && m_window_open) {
            if (ImGui::Begin("AltDPad", &m_window_open)) {
                ImGui::Checkbox("Enable Alt DPad", &m_enabled);
                int method = (int)m_method;
                if (ImGui::Combo("Mode", &method, ALT_DPAD_METHOD_NAMES, IM_ARRAYSIZE(ALT_DPAD_METHOD_NAMES))) {
                    m_method = (AltDPadMethod)method;
                }
                ImGui::End();
            }
        }

        ImGui::EndFrame();
        ImGui::Render();

        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            g_d3d11.render_imgui();
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            g_d3d12.render_imgui();
        }
    }

    void on_xinput_get_state(uint32_t* retval, uint32_t /*user_index*/, XINPUT_STATE* state) override {
        if (*retval != ERROR_SUCCESS || !m_enabled) {
            return;
        }

        if (m_method == AltDPadMethod::NONE) {
            return;
        }

        auto left_src = API::VR::get_left_joystick_source();
        auto right_src = API::VR::get_right_joystick_source();

        bool left_click = API::VR::is_action_active(m_joystick_click, left_src);
        bool right_click = API::VR::is_action_active(m_joystick_click, right_src);

        auto left_axis = API::VR::get_joystick_axis(left_src);
        auto right_axis = API::VR::get_joystick_axis(right_src);

        bool active = false;
        UEVR_Vector2f axis{};

        if (m_method == AltDPadMethod::RIGHT_CLICK_LEFT_STICK && right_click && !left_click) {
            axis = left_axis;
            active = true;
            state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_RIGHT_THUMB;
            state->Gamepad.sThumbLX = 0;
            state->Gamepad.sThumbLY = 0;
        } else if (m_method == AltDPadMethod::LEFT_CLICK_RIGHT_STICK && left_click && !right_click) {
            axis = right_axis;
            active = true;
            state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_THUMB;
            state->Gamepad.sThumbRX = 0;
            state->Gamepad.sThumbRY = 0;
        }

        if (active) {
            if (axis.y >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            }
            if (axis.y <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            }
            if (axis.x >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            }
            if (axis.x <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            }
        }
    }

private:
    bool initialize_imgui() {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        static const auto imgui_ini = API::get()->get_persistent_dir(L"altdpad_plugin.ini").string();
        ImGui::GetIO().IniFilename = imgui_ini.c_str();

        const auto renderer_data = API::get()->param()->renderer;
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        auto swapchain = (IDXGISwapChain*)renderer_data->swapchain;
        swapchain->GetDesc(&swap_desc);
        m_wnd = swap_desc.OutputWindow;
        if (!ImGui_ImplWin32_Init(m_wnd)) {
            return false;
        }
        if (renderer_data->renderer_type == UEVR_RENDERER_D3D11) {
            if (!g_d3d11.initialize()) {
                return false;
            }
        } else if (renderer_data->renderer_type == UEVR_RENDERER_D3D12) {
            if (!g_d3d12.initialize()) {
                return false;
            }
        }
        m_initialized = true;
        return true;
    }

private:
    HWND m_wnd{};
    bool m_initialized{false};
    std::recursive_mutex m_imgui_mutex{};
    AltDPadMethod m_method{AltDPadMethod::NONE};
    bool m_enabled{true};
    bool m_window_open{true};
    bool m_prev_main_menu_open{false};
    UEVR_ActionHandle m_joystick_click{};
};

std::unique_ptr<AltDPadPlugin> g_plugin{std::make_unique<AltDPadPlugin>()};

} // anonymous namespace
