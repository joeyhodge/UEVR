#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "ScriptContext.hpp"

namespace {
using API = uevr::API;

unsigned int name_size = 8;
bool null_names = false;
std::array<int32_t, 3> object_name{1, 0, 101};
std::array<int32_t, 3> field_name{2, 0, 102};
std::array<int32_t, 3> field_class_name{3, 0, 103};
std::array<int32_t, 3> property_name{4, 0, 104};
constexpr std::array<std::wstring_view, 6> texts{
    L"None", L"ObjectName", L"FieldName", L"FieldClassName", L"PropertyName", L"OwnedName"};

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename T>
bool register_callback(T) { return true; }

void log_message(const char*, ...) {}

UEVR_FNameHandle borrowed(std::array<int32_t, 3>& name) {
    return null_names ? nullptr : reinterpret_cast<UEVR_FNameHandle>(name.data());
}

unsigned int name_to_string(UEVR_FNameHandle name, wchar_t* buffer, unsigned int capacity) {
    require(name != nullptr, "name conversion must not receive a null pointer");
    int32_t index{};
    std::memcpy(&index, name, sizeof(index));
    require(index >= 0 && index < texts.size(), "name conversion receives valid fixture storage");
    const auto text = texts[index];
    if (buffer) {
        require(capacity > text.size(), "name output has space for its terminator");
        std::memcpy(buffer, text.data(), text.size() * sizeof(wchar_t));
        buffer[text.size()] = L'\0';
    }
    return static_cast<unsigned int>(text.size());
}

bool copy_name(void* destination, unsigned int capacity, UEVR_FNameHandle source) {
    if (!destination || !source || capacity < name_size) {
        return false;
    }
    std::memcpy(destination, source, name_size);
    return true;
}

bool construct_name(void* destination, unsigned int capacity, const wchar_t* text, unsigned int) {
    if (!destination || !text || capacity < name_size) {
        return false;
    }
    require(std::wstring_view{text} == L"OwnedName", "owned construction receives expected text");
    const std::array<int32_t, 3> value{5, 0, 105};
    std::memcpy(destination, value.data(), name_size);
    return true;
}

struct MockSDK {
    UEVR_PluginVersion version{2, 40, 0};
    UEVR_PluginFunctions functions{};
    UEVR_PluginCallbacks callbacks{};
    UEVR_SDKCallbacks sdk_callbacks{};
    UEVR_UObjectFunctions objects{};
    UEVR_FFieldFunctions fields{};
    UEVR_FFieldClassFunctions field_classes{};
    UEVR_FNameFunctions names{};
    UEVR_OwnedFNameFunctions owned_names{};
    UEVR_SDKData sdk{};
    UEVR_LuaData lua{};
    UEVR_PluginInitializeParam params{};

    explicit MockSDK(bool legacy) {
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
        objects.get_fname = [](UEVR_UObjectHandle) { return borrowed(object_name); };
        objects.get_property_data = [](UEVR_UObjectHandle, const wchar_t* name) -> void* {
            return std::wstring_view{name} == L"NameProperty" ? property_name.data() : nullptr;
        };
        fields.get_fname = [](UEVR_FFieldHandle) { return borrowed(field_name); };
        field_classes.get_fname = [](UEVR_FFieldClassHandle) { return borrowed(field_class_name); };
        names.to_string = name_to_string;
        owned_names.get_size = [] { return name_size; };
        owned_names.construct = construct_name;
        owned_names.copy = copy_name;
        sdk.callbacks = &sdk_callbacks;
        sdk.uobject = &objects;
        sdk.ffield = &fields;
        sdk.ffield_class = &field_classes;
        sdk.fname = &names;
        sdk.owned_fname = legacy ? nullptr : &owned_names;
        version.minor = legacy ? 39 : 40;
        lua.add_additional_bindings = [](lua_State*) {};
        params.version = &version;
        params.functions = &functions;
        params.callbacks = &callbacks;
        params.sdk = &sdk;
        params.lua = &lua;
    }
};

void run_script(sol::state& lua, const char* script) {
    auto result = lua.safe_script(script, sol::script_pass_on_error);
    if (!result.valid()) {
        const sol::error error = result;
        throw std::runtime_error{error.what()};
    }
}

void test_bindings(MockSDK& mock, bool legacy) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string);
    // Exercise the production registration used by both LuaLoader and LuaVR,
    // not a duplicate registration that could drift away from ScriptContext.
    auto context = uevr::ScriptContext::create(lua.lua_state(), &mock.params);
    require(context->setup_bindings() == 1, "binding setup returns the public API table");
    lua["uevr"] = sol::stack::pop<sol::table>(lua);

    API::UObject object{};
    API::FField field{};
    API::FFieldClass field_class{};
    lua["object"] = sol::make_object(lua, &object);
    lua["field"] = sol::make_object(lua, &field);
    lua["field_class"] = sol::make_object(lua, &field_class);
    run_script(lua, R"(
        assert(object:get_fname():to_string() == "ObjectName")
        assert(field:get_fname():to_string() == "FieldName")
        assert(field_class:get_fname():to_string() == "FieldClassName")
        assert(object:get_short_name() == object:get_fname():to_string())
        assert(field_class:get_name() == field_class:get_fname():to_string())
        assert(uevr.types.FName == UEVR_FName)
        assert(UEVR_FNameRef ~= UEVR_FName)
        assert(not pcall(function() return UEVR_FNameRef.new() end))
        assert(not pcall(function() return UEVR_FNameRef() end))
        borrowed_name = object:get_fname()
        collectgarbage("collect")
        assert(borrowed_name:to_string() == "ObjectName")
    )");
    require(lua.get<sol::object>("borrowed_name").is<API::FName*>(), "get_fname remains a borrowed pointer");

    null_names = true;
    run_script(lua, R"(
        assert(object:get_fname() == nil)
        assert(field:get_fname() == nil)
        assert(field_class:get_fname() == nil)
    )");
    null_names = false;

    if (legacy) {
        run_script(lua, R"(
            local ok, err = pcall(function() return object:get_fname_property("NameProperty") end)
            assert(not ok and string.find(err, "requires SDK 2.40", 1, true))
        )");
        return;
    }

    lua["constructed_name"] = API::OwnedFName{L"OwnedName"};
    run_script(lua, R"(
        assert(constructed_name:to_string() == "OwnedName")
        owned_name = object:get_fname_property("NameProperty")
        assert(owned_name:to_string() == "PropertyName")
        assert(not pcall(function() return object:get_fname_property("MissingProperty") end))
        collectgarbage("collect")
        assert(owned_name:to_string() == "PropertyName")
    )");
    auto value = lua.get<sol::object>("owned_name");
    require(value.is<API::OwnedFName>(), "NameProperty keeps owned storage");
    const auto owned = value.as<API::OwnedFName>();
    require(std::memcmp(owned.words, property_name.data(), name_size) == 0, "owned copy retains the complete runtime name");
    property_name[0] = 1;
    run_script(lua, R"(assert(owned_name:to_string() == "PropertyName"))");
    property_name[0] = 4;

    std::array<int32_t, 4> output{-1, -1, -1, -1};
    require(!owned.write_to(output.data(), name_size - 1), "undersized owned writes remain rejected");
    require(output == std::array<int32_t, 4>{-1, -1, -1, -1}, "failed owned write leaves destination intact");
    require(owned.write_to(output.data(), name_size), "sized owned write succeeds");
    require(std::memcmp(output.data(), property_name.data(), name_size) == 0, "owned write preserves runtime data");
    require(output[name_size / sizeof(int32_t)] == -1, "owned write preserves trailing canary");
}
}

int main(int argc, char** argv) {
    try {
        require(argc >= 2, "pass the runtime FName width");
        name_size = static_cast<unsigned int>(std::stoul(argv[1]));
        require(name_size == 8 || name_size == 12, "runtime FName width must be eight or twelve bytes");
        const bool legacy = argc == 3 && std::string_view{argv[2]} == "legacy";
        MockSDK mock{legacy};
        test_bindings(mock, legacy);
        test_bindings(mock, legacy); // Fresh Lua state, as on script reload.
        std::cout << "Lua FName bindings passed (" << name_size << " bytes, SDK 2." << mock.version.minor << ")\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << e.what() << '\n';
        return 1;
    }
}
