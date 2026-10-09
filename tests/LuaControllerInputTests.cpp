#define NOMINMAX
#include <Windows.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "ScriptContext.hpp"

namespace {
using API = uevr::API;
static_assert(offsetof(UEVR_VRData, get_controller_type) == 47 * sizeof(void*));
static_assert(sizeof(UEVR_VRData) == 51 * sizeof(void*));

void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error{message}; }
}
template<typename T> bool register_callback(T) { return true; }
void log_message(const char*, ...) {}

struct MockInput {
    static inline unsigned calls{};
    static inline bool grip_pressed{}, grip_active{true};
    static inline bool swapped{};
    static inline bool openxr{}, runtime_ready{true};
    static inline UEVR_InputSourceHandle observed_source{};
    static inline bool bumper_active{true};
    static inline UEVR_InputSourceHandle left_source{(UEVR_InputSourceHandle)10}, right_source{(UEVR_InputSourceHandle)11};
    static inline std::string left_type{"frame"}, right_type{"frame"};
    UEVR_PluginVersion version{2, 41, 0};
    UEVR_PluginFunctions functions{};
    UEVR_PluginCallbacks callbacks{};
    UEVR_SDKCallbacks sdk_callbacks{};
    UEVR_SDKData sdk{};
    UEVR_LuaData lua{};
    UEVR_VRData vr{};
    UEVR_PluginInitializeParam params{};
    void* legacy_storage{};

    explicit MockInput(bool legacy, bool missing) {
        functions.log_info = log_message;
        functions.log_warn = log_message;
        functions.log_error = log_message;
        functions.remove_callback = [](void*) { return true; };
        callbacks.on_xinput_get_state = register_callback<UEVR_OnXInputGetStateCb>;
        callbacks.on_xinput_set_state = register_callback<UEVR_OnXInputSetStateCb>;
        sdk_callbacks.on_pre_engine_tick = register_callback<UEVR_Engine_TickCb>;
        sdk_callbacks.on_post_engine_tick = register_callback<UEVR_Engine_TickCb>;
        sdk_callbacks.on_pre_slate_draw_window_render_thread = register_callback<UEVR_Slate_DrawWindow_RenderThreadCb>;
        sdk_callbacks.on_post_slate_draw_window_render_thread = register_callback<UEVR_Slate_DrawWindow_RenderThreadCb>;
        sdk_callbacks.on_early_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_pre_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_post_calculate_stereo_view_offset = register_callback<UEVR_Stereo_CalculateStereoViewOffsetCb>;
        sdk_callbacks.on_pre_viewport_client_draw = register_callback<UEVR_ViewportClient_DrawCb>;
        sdk_callbacks.on_post_viewport_client_draw = register_callback<UEVR_ViewportClient_DrawCb>;
        sdk.callbacks = &sdk_callbacks;
        params.version = &version;
        params.functions = &functions;
        params.callbacks = &callbacks;
        params.sdk = &sdk;
        params.lua = &lua;
        lua.add_additional_bindings = [](lua_State*) {};
        vr.is_runtime_ready = [] { return runtime_ready; };
        vr.is_openxr = [] { return openxr; };
        vr.is_openvr = [] { return !openxr; };
        vr.get_left_joystick_source = [] { return swapped ? right_source : left_source; };
        vr.get_right_joystick_source = [] { return swapped ? left_source : right_source; };
        vr.is_action_active = [](UEVR_ActionHandle, UEVR_InputSourceHandle source) {
            observed_source = source;
            return source == left_source || source == right_source;
        };
        vr.get_joystick_axis = [](UEVR_InputSourceHandle source, UEVR_Vector2f* out) {
            observed_source = source;
            *out = {source == left_source ? 0.25f : 0.75f, 0.5f};
        };
        vr.trigger_haptic_vibration = [](float, float, float, float, UEVR_InputSourceHandle source) {
            observed_source = source;
        };
        vr.get_action_handle = [](const char* name) {
            return reinterpret_cast<UEVR_ActionHandle>(static_cast<uintptr_t>(std::string_view{name}.ends_with("Grip") ? 20 :
                std::string_view{name}.ends_with("Bumper") ? 21 : 22));
        };
        if (!missing) {
            vr.get_controller_type = [](UEVR_InputSourceHandle source) {
                ++calls;
                return source == left_source ? left_type.c_str() : source == right_source ? right_type.c_str() : "unknown";
            };
            vr.get_controller_profile = [](UEVR_InputSourceHandle source, char* buffer, unsigned int capacity) {
                ++calls;
                const char* value = source == left_source ? "frame_controller" :
                    source == right_source ? "/interaction_profiles/valve/frame_controller_valve" : "";
                const auto size = static_cast<unsigned int>(std::strlen(value));
                if (buffer && capacity > size) { std::memcpy(buffer, value, size + 1); }
                return size;
            };
            vr.get_action_state = [](UEVR_ActionHandle action, UEVR_InputSourceHandle source, UEVR_DigitalInputState* out) {
                ++calls;
                const bool right = source == right_source;
                const bool frame = (right ? right_type : left_type) == "frame";
                *out = {action == (UEVR_ActionHandle)21 ? frame && bumper_active :
                    right && (action == (UEVR_ActionHandle)20 ? grip_active : action == (UEVR_ActionHandle)22 && frame),
                    action == (UEVR_ActionHandle)20 && grip_pressed};
            };
            vr.get_action_axis = [](UEVR_ActionHandle action, UEVR_InputSourceHandle source, UEVR_AnalogInputState* out) {
                ++calls;
                const bool active = action != nullptr && source == right_source;
                *out = {active, action == (UEVR_ActionHandle)22 ? std::numeric_limits<float>::quiet_NaN() :
                    action == (UEVR_ActionHandle)23 ? 2.0f : active ? 0.75f : 1.0f};
            };
        }
        params.vr = &vr;
        if (legacy) {
            version.minor = 40;
            SYSTEM_INFO info{};
            GetSystemInfo(&info);
            legacy_storage = VirtualAlloc(nullptr, info.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            require(legacy_storage != nullptr, "legacy API guard allocation");
            DWORD old{};
            require(VirtualProtect((char*)legacy_storage + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old), "legacy API guard page");
            // Place the real 2.40 prefix directly against inaccessible tail storage.
            auto prefix = (char*)legacy_storage + info.dwPageSize - offsetof(UEVR_VRData, get_controller_type);
            std::memcpy(prefix, &vr, offsetof(UEVR_VRData, get_controller_type));
            params.vr = reinterpret_cast<UEVR_VRData*>(prefix);
        }
    }
    ~MockInput() { if (legacy_storage) { VirtualFree(legacy_storage, 0, MEM_RELEASE); } }
};

void script(sol::state& lua, const std::string& source) {
    const auto result = lua.safe_script(source, sol::script_pass_on_error);
    if (!result.valid()) { const sol::error error = result; throw std::runtime_error{error.what()}; }
}
}

int main(int argc, char** argv) {
    try {
        require(argc >= 3, "Pass native/openxr-sources/legacy/missing and the source root");
        const auto mode = std::string_view{argv[1]};
        require(mode == "native" || mode == "openxr-sources" || mode == "legacy" || mode == "missing", "Unknown input test mode");
        if (mode == "openxr-sources") {
            MockInput::openxr = true;
            MockInput::left_source = nullptr; // OpenXR's physical left hand is source zero.
            MockInput::right_source = (UEVR_InputSourceHandle)1;
        }
        const bool native = mode == "native" || mode == "openxr-sources";
        MockInput mock{mode == "legacy", mode == "missing"};
        sol::state lua;
        lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string);
        if (argc >= 4) {
            const auto compiled = lua.load_file(argv[3]);
            require(compiled.valid(), "Optional profile compiles without executing game callbacks");
        }
        auto context = uevr::ScriptContext::create(lua.lua_state(), &mock.params);
        require(context->setup_bindings() == 1, "production Lua registration");
        lua["uevr"] = sol::stack::pop<sol::table>(lua);
        lua["native"] = native;
        lua["left"] = MockInput::left_source;
        lua["right"] = MockInput::right_source;
        lua["grip"] = (UEVR_ActionHandle)20;
        lua["nan_axis"] = (UEVR_ActionHandle)22;
        lua["large_axis"] = (UEVR_ActionHandle)23;
        UEVR_Vector2f axis_out{};
        lua["axis_out"] = &axis_out;
        script(lua, R"(
            local vr = uevr.params.vr
            left_getter, right_getter = vr.get_left_joystick_source, vr.get_right_joystick_source
            assert(type(left_getter) == 'function' and type(right_getter) == 'function')
            local source = left_getter()
            assert(source ~= nil and right_getter() ~= nil)
            assert(vr.is_action_active(grip, source))
            vr.get_joystick_axis(source, axis_out)
            assert(axis_out.x == 0.25 and axis_out.y == 0.5)
            vr.trigger_haptic_vibration(0, 0.1, 0, 0.25, source)
        )");
        require(MockInput::observed_source == MockInput::left_source, "Lua source round-trips through legacy input/haptic consumers");
        script(lua, R"(
            vr = uevr.params.vr
            assert(vr.get_controller_type(left) == (native and 'frame' or 'unknown'))
            assert(vr.get_controller_profile(left) == (native and 'frame_controller' or ''))
            assert(vr.get_controller_profile(right) == (native and '/interaction_profiles/valve/frame_controller_valve' or ''))
            local axis = vr.get_action_axis(grip, right)
            assert(axis.active == native and axis.value == (native and 0.75 or 0))
            local unavailable = vr.get_action_axis(grip, left)
            assert(not unavailable.active and unavailable.value == 0)
            local bad = vr.get_action_axis(nan_axis, right)
            assert(not bad.active and bad.value == 0)
            assert(vr.get_action_axis(large_axis, right).value == (native and 1 or 0))
            assert(not vr.get_action_state(grip, left).pressed)
            assert(not vr.get_action_state(nil, right).active)
            assert(not vr.get_action_axis(nil, right).active)
        )");
        if (!native) { require(MockInput::calls == 0, "old/missing API never calls an appended entry"); }
        require(API::VR::get_controller_type(MockInput::left_source) == (native ? "frame" : "unknown"), "C++ facade uses the same version guard");
        if (mode == "openxr-sources") {
            script(lua, R"(
                local vr = uevr.params.vr
                assert(left_getter() ~= nil and left_getter() == left_getter())
                assert(vr.get_controller_type(left_getter()) == 'frame')
                assert(vr.get_controller_profile(left_getter()) == 'frame_controller')
                assert(not vr.get_action_state(grip, left_getter()).pressed)
                assert(not vr.get_action_axis(grip, left_getter()).active)
                assert(vr.get_controller_type(nil) == 'frame') -- old scripts can still pass the zero source
                assert(vr.is_action_active(grip, nil))
                vr.get_joystick_axis(nil, axis_out)
                assert(axis_out.x == 0.25 and axis_out.y == 0.5)
                vr.trigger_haptic_vibration(0, 0.1, 0, 0.25, nil)
            )");
            MockInput::runtime_ready = false;
            script(lua, "assert(left_getter() == nil and right_getter() ~= nil)");
            MockInput::runtime_ready = true;
            MockInput::openxr = false;
            script(lua, "assert(left_getter() == nil)"); // missing OpenVR source remains nil
            MockInput::openxr = true;
            MockInput::swapped = true;
            script(lua, R"(
                local vr = uevr.params.vr
                assert(vr.get_controller_profile(left_getter()) == '/interaction_profiles/valve/frame_controller_valve')
                assert(right_getter() ~= nil and vr.get_controller_profile(right_getter()) == 'frame_controller')
                assert(vr.is_action_active(grip, right_getter()))
                vr.get_joystick_axis(right_getter(), axis_out)
                assert(axis_out.x == 0.25 and axis_out.y == 0.5)
                vr.trigger_haptic_vibration(0, 0.1, 0, 0.25, right_getter())
            )");
            require(MockInput::observed_source == MockInput::left_source, "swapped zero source stays the physical left hand");
            MockInput::swapped = false;
        }

        const auto module_path = std::filesystem::path{argv[2]} / "examples/lua/steam_frame_satisfactory.lua";
        std::ifstream module{module_path};
        require(static_cast<bool>(module), "Satisfactory adapter fixture exists");
        const std::string source{std::istreambuf_iterator<char>{module}, {}};
        script(lua, "adapter = (function()\n" + source + "\nend)()");
        script(lua, R"(
            local result = adapter.buttons(vr, {wButtons=0x2000 | 0x0100 | 0x0200})
            assert(result.b == native and result.x == not native)
            assert(result.left_grip == not native and result.right_grip == not native)
            local same = adapter.buttons(vr, {wButtons=0x2000 | 0x0100 | 0x0200})
            assert(not adapter.changed(result, same))
            local sticks = adapter.buttons(vr, {wButtons=0x0040 | 0x0080})
            assert(sticks.left_stick and sticks.right_stick)
            assert(not adapter.buttons(vr, {wButtons=0}).right_stick)
        )");
        if (native) {
            MockInput::grip_pressed = true;
            script(lua, R"(
                local released = {right_grip=false}
                local result = adapter.buttons(vr, {wButtons=0})
                assert(result.right_grip and not result.left_grip)
                assert(adapter.changed(result, released)) -- grip-only press, no XInput bit change
            )");
            MockInput::grip_active = false;
            script(lua, "assert(not adapter.buttons(vr, {wButtons=0x0200}).right_grip)");
            MockInput::left_type = "touch";
            script(lua, R"(
                local mixed = adapter.buttons(vr, {wButtons=0x0100 | 0x0200 | 0x4000})
                assert(mixed.left_grip and not mixed.right_grip and mixed.x and not mixed.b)
            )");
            MockInput::right_type = "touch";
            script(lua, R"(
                local fallback = adapter.buttons(vr, {wButtons=0x0200 | 0x4000})
                assert(fallback.right_grip and fallback.b and not fallback.x)
            )");
            MockInput::left_type = "frame";
            MockInput::swapped = true;
            script(lua, R"(
                local mixed_swap = adapter.buttons(vr, {wButtons=0x4000})
                assert(mixed_swap.b and not mixed_swap.x) -- only Frame's left controller is present
            )");
            MockInput::right_type = "frame";
            script(lua, R"(
                local frame_swap = adapter.buttons(vr, {wButtons=0x2000})
                assert(frame_swap.b and not frame_swap.x)
            )");
            MockInput::bumper_active = false;
            script(lua, R"(
                local old_binding = adapter.buttons(vr, {wButtons=0x2000 | 0x0200})
                assert(old_binding.x and not old_binding.b and old_binding.right_grip)
            )");
        }
        std::cout << "Lua controller input passed: " << mode << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
