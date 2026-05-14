#define NOMINMAX

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include <utility/Config.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>

#include <sdk/CVar.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/Utility.hpp>

#include "Framework.hpp"

#include "CVarManager.hpp"

#include <tracy/Tracy.hpp>

constexpr std::string_view cvars_standard_txt_name = "cvars_standard.txt";
constexpr std::string_view cvars_data_txt_name = "cvars_data.txt";
constexpr std::string_view user_script_txt_name = "user_script.txt";

namespace {
bool is_ue_5_1_dx12_backend_for_cvars() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const bool is_ue_5_1 = []() {
        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (found_version) {
            const auto version = utility::narrow(*found_version);
            return version == "5.1" || version.starts_with("5.1.");
        }

        const auto disk_version = sdk::get_file_version_info();
        return disk_version.dwFileVersionMS == 0x00050001;
    }();

    return is_ue_5_1;
}

bool is_stalker2_current_game_for_cvars() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_aphelion_current_game_for_cvars() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path &&
            (exe_path->find(L"PIO-WinGDK-Shipping") != std::wstring::npos ||
             exe_path->find(L"PIO-Win64-Shipping") != std::wstring::npos ||
             exe_path->find(L"Aphelion") != std::wstring::npos);
    }();

    return result;
}

bool is_windrose_ue56_dx12_current_game_for_cvars() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path || exe_path->find(L"Windrose-Win64-Shipping") == std::wstring::npos) {
            return false;
        }

        if (const auto found_version = sdk::search_for_version(utility::get_executable())) {
            const auto version = utility::narrow(*found_version);
            return version == "5.6" || version.starts_with("5.6.");
        }

        const auto disk_version = sdk::get_file_version_info();
        return disk_version.dwFileVersionMS == 0x00050006;
    }();

    return result;
}

bool propagate_alpha_allows_tonemapper_value() {
    static const bool result = []() {
        int major{};
        int minor{};

        if (const auto found_version = sdk::search_for_version(utility::get_executable())) {
            if (swscanf_s(found_version->c_str(), L"%d.%d", &major, &minor) == 2) {
                return major < 5 || (major == 5 && minor <= 4);
            }
        }

        const auto disk_version = sdk::get_file_version_info();
        major = (int)((disk_version.dwFileVersionMS >> 16) & 0xffff);
        minor = (int)(disk_version.dwFileVersionMS & 0xffff);

        return major < 5 || (major == 5 && minor <= 4);
    }();

    return result;
}

bool force_ue51_fsr3_runtime_cvars_once(int attempt) {
    if (!is_ue_5_1_dx12_backend_for_cvars() || !is_stalker2_current_game_for_cvars()) {
        return true;
    }

    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return false;
    }

    const auto has_fsr3_plugin_cvars =
        console_manager->find(L"r.FidelityFX.FSR3.Enabled") != nullptr ||
        console_manager->find(L"r.FidelityFX.FI.OverrideSwapChainDX12") != nullptr;

    if (!has_fsr3_plugin_cvars) {
        return false;
    }

    struct ForcedCVar {
        const wchar_t* name;
        const wchar_t* value;
    };

    // Stalker2's current UE 5.1 build can crash on level load when D3D12
    // closes a copy command list containing occlusion query work.
    static constexpr std::array forced_cvars{
        ForcedCVar{L"r.AllowOcclusionQueries", L"0"},
    };

    int found{};
    int set_ok{};
    int set_failed{};
    int missing{};

    for (const auto& forced : forced_cvars) {
        auto object = console_manager->find(forced.name);

        if (object == nullptr) {
            ++missing;
            SPDLOG_INFO("[UE5.1][DX12] cvar missing: {}", utility::narrow(forced.name));
            continue;
        }

        ++found;
        auto variable = (sdk::IConsoleVariable*)object;

        int before{};
        int after{};
        bool ok{};

        try {
            before = variable->GetInt();
            ok = variable->Set(forced.value);
            after = variable->GetInt();
        } catch (...) {
            ok = false;
        }

        if (ok) {
            ++set_ok;
        } else {
            ++set_failed;
        }

        SPDLOG_INFO(
            "[UE5.1][DX12] forced {}: before={} requested={} after={} ok={}",
            utility::narrow(forced.name),
            before,
            utility::narrow(forced.value),
            after,
            ok);
    }

    SPDLOG_INFO(
        "[UE5.1][DX12] runtime cvar pass complete attempt={} found={} missing={} set_ok={} set_failed={}",
        attempt,
        found,
        missing,
        set_ok,
        set_failed);

    return true;
}

bool force_aphelion_framegen_runtime_cvars_once(int attempt) {
    if (g_framework == nullptr || !g_framework->is_dx12() || !is_aphelion_current_game_for_cvars()) {
        return true;
    }

    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return false;
    }

    struct ForcedCVar {
        const wchar_t* name;
        const wchar_t* value;
    };

    // Aphelion ships both Streamline/DLSSG and FSR3 frame-interpolation paths.
    // In VR these custom-present/frame-generation layers can fight UEVR's OpenXR
    // submission and produce gameplay-only stalls or one-frame camera-cut ghosting.
    static constexpr std::array forced_cvars{
        ForcedCVar{L"r.FidelityFX.FI.Enabled", L"0"},
        ForcedCVar{L"r.FidelityFX.FI.OverrideSwapChainDX12", L"0"},
        ForcedCVar{L"r.FidelityFX.FI.RHIPacingMode", L"0"},
        ForcedCVar{L"r.Streamline.DLSSG.Enable", L"0"},
        ForcedCVar{L"r.Streamline.Latewarp.Enable", L"0"},
        ForcedCVar{L"t.Streamline.Reflex.Enable", L"0"},
        ForcedCVar{L"t.Streamline.Reflex.Auto", L"0"},
        ForcedCVar{L"t.Streamline.Reflex.Mode", L"0"},
        ForcedCVar{L"r.Streamline.Reflex.PredictiveRendering", L"0"},
    };

    int found{};
    int set_ok{};
    int set_failed{};
    int missing{};

    for (const auto& forced : forced_cvars) {
        auto object = console_manager->find(forced.name);

        if (object == nullptr) {
            ++missing;
            continue;
        }

        ++found;
        auto variable = (sdk::IConsoleVariable*)object;

        int before{};
        int after{};
        bool ok{};

        try {
            before = variable->GetInt();
            ok = variable->Set(forced.value);
            after = variable->GetInt();
        } catch (...) {
            ok = false;
        }

        if (ok) {
            ++set_ok;
        } else {
            ++set_failed;
        }

        SPDLOG_INFO(
            "[Aphelion][DX12] forced {}: before={} requested={} after={} ok={}",
            utility::narrow(forced.name),
            before,
            utility::narrow(forced.value),
            after,
            ok);
    }

    if (found == 0) {
        return false;
    }

    SPDLOG_INFO(
        "[Aphelion][DX12] frame-generation cvar pass complete attempt={} found={} missing={} set_ok={} set_failed={}",
        attempt,
        found,
        missing,
        set_ok,
        set_failed);

    return true;
}

bool force_windrose_shadow_runtime_cvars_once(int attempt) {
    if (!is_windrose_ue56_dx12_current_game_for_cvars()) {
        return true;
    }

    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return false;
    }

    struct ForcedCVar {
        const wchar_t* name;
        const wchar_t* value;
    };

    // Windrose/R5 crashes in UE5.6 distance-field/heightfield shadow RDG work
    // when UEVR is active. This is deliberately scoped to crash avoidance only;
    // it does not try to fix the separate HUD-only black-scene renderer issue.
    static constexpr std::array forced_cvars{
        ForcedCVar{L"r.DistanceFieldShadowing", L"0"},
        ForcedCVar{L"r.HeightFieldShadowing", L"0"},
        ForcedCVar{L"r.DFShadowQuality", L"0"},
        ForcedCVar{L"r.HFShadowQuality", L"0"},
        ForcedCVar{L"r.DFShadowAsyncCompute", L"0"},
    };

    int found{};
    int set_ok{};
    int set_failed{};
    int missing{};

    for (const auto& forced : forced_cvars) {
        auto object = console_manager->find(forced.name);

        if (object == nullptr) {
            ++missing;
            continue;
        }

        ++found;
        auto variable = (sdk::IConsoleVariable*)object;

        int before{};
        int after{};
        bool ok{};

        try {
            before = variable->GetInt();
            ok = variable->Set(forced.value);
            after = variable->GetInt();
        } catch (...) {
            ok = false;
        }

        if (ok) {
            ++set_ok;
        } else {
            ++set_failed;
        }

        SPDLOG_INFO(
            "[Windrose][UE5.6][ShadowCrash] forced {}: before={} requested={} after={} ok={}",
            utility::narrow(forced.name),
            before,
            utility::narrow(forced.value),
            after,
            ok);
    }

    if (found == 0) {
        return false;
    }

    SPDLOG_INFO(
        "[Windrose][UE5.6][ShadowCrash] cvar pass complete attempt={} found={} missing={} set_ok={} set_failed={}",
        attempt,
        found,
        missing,
        set_ok,
        set_failed);

    return true;
}
}

CVarManager::CVarManager() {
    ZoneScopedN(__FUNCTION__);

    m_displayed_cvars.insert(m_displayed_cvars.end(), s_default_standard_cvars.begin(), s_default_standard_cvars.end());
    m_displayed_cvars.insert(m_displayed_cvars.end(), s_default_data_cvars.begin(), s_default_data_cvars.end());

    // Sort first by name, then by bool/int/float type. Bools get displayed first.
    std::sort(m_displayed_cvars.begin(), m_displayed_cvars.end(), [](const auto& a, const auto& b) {
        return a->get_name() < b->get_name();
    });

    std::sort(m_displayed_cvars.begin(), m_displayed_cvars.end(), [](const auto& a, const auto& b) {
        return (int)a->get_type() < (int)b->get_type();
    });

    m_all_cvars.insert(m_all_cvars.end(), m_displayed_cvars.begin(), m_displayed_cvars.end());

    // set m_hzbo (shared ptr) to the r.HZBOcclusion cvar in m_all_cvars
    for (auto& cvar : m_all_cvars) {
        if (cvar->get_name() == L"r.HZBOcclusion") {
            m_hzbo = cvar;
            break;
        }
    }
}

CVarManager::~CVarManager() {
    ZoneScopedN(__FUNCTION__);

    /*for (auto& cvar : m_cvars) {
        cvar->save();
    }*/
}

CVarManager::ChangeSnapshot CVarManager::get_change_snapshot() const {
    std::scoped_lock _{s_change_mutex};
    return s_change_snapshot;
}

uint64_t CVarManager::get_change_counter() const {
    return s_change_counter.load(std::memory_order_relaxed);
}

void CVarManager::record_global_change(std::wstring_view name, std::wstring_view value, std::string_view source) {
    const auto counter = s_change_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    std::scoped_lock _{s_change_mutex};
    s_change_snapshot.counter = counter;
    s_change_snapshot.name = utility::narrow(std::wstring{name});
    s_change_snapshot.value = utility::narrow(std::wstring{value});
    s_change_snapshot.source = std::string{source};
}

void CVarManager::record_global_command(std::string_view command, std::string_view source) {
    const auto counter = s_change_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    std::scoped_lock _{s_change_mutex};
    s_change_snapshot.counter = counter;
    s_change_snapshot.name = "<console_command>";
    s_change_snapshot.value = std::string{command};
    s_change_snapshot.source = std::string{source};
}

void CVarManager::refresh_frozen_cvar_state() {
    m_has_frozen_cvars = std::any_of(m_all_cvars.begin(), m_all_cvars.end(), [](const auto& cvar) {
        return cvar->is_frozen();
    });
}

void CVarManager::spawn_console() {
    if (m_native_console_spawned) {
        return;
    }

    // Find Engine object and add the Console
    const auto engine = sdk::UGameEngine::get();

    if (engine != nullptr) {
        const auto console_class = engine->get_property<sdk::UClass*>(L"ConsoleClass");
        auto game_viewport = engine->get_property<sdk::UObject*>(L"GameViewport");

        if (console_class != nullptr && game_viewport != nullptr) {
            const auto console = sdk::UGameplayStatics::get()->spawn_object(console_class, game_viewport);

            if (console != nullptr) {
                game_viewport->get_property<sdk::UObject*>(L"ViewportConsole") = console;
                m_native_console_spawned = true;
            }
        }
    }
}

void CVarManager::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    const bool process_all_cvars = m_needs_full_refresh || m_cvar_ui_open_this_frame;

    if (process_all_cvars) {
        for (auto& cvar : m_all_cvars) {
            cvar->update();
        }
    } else if (m_has_frozen_cvars) {
        for (auto& cvar : m_all_cvars) {
            if (cvar->is_frozen()) {
                cvar->update();
            }
        }
    }

    if (m_has_frozen_cvars) {
        for (auto& cvar : m_all_cvars) {
            if (process_all_cvars || cvar->is_frozen()) {
                cvar->freeze();
            }
        }
    }

    m_needs_full_refresh = false;

    if (m_should_execute_console_script) {
        execute_console_script(engine, user_script_txt_name.data());
        m_should_execute_console_script = false;
        m_ue51_fsr3_runtime_cvars_done = false;
        m_ue51_fsr3_runtime_cvar_attempts = 0;
        m_aphelion_framegen_runtime_cvars_done = false;
        m_aphelion_framegen_runtime_cvar_attempts = 0;
        m_windrose_shadow_runtime_cvars_done = false;
        m_windrose_shadow_runtime_cvar_attempts = 0;
    }

    if (!m_ue51_fsr3_runtime_cvars_done) {
        ++m_ue51_fsr3_runtime_cvar_attempts;

        if (force_ue51_fsr3_runtime_cvars_once(m_ue51_fsr3_runtime_cvar_attempts)) {
            m_ue51_fsr3_runtime_cvars_done = true;
        } else if (m_ue51_fsr3_runtime_cvar_attempts >= 600) {
            SPDLOG_WARN("[UE5.1][DX12] runtime cvars were not found after {} attempts; giving up", m_ue51_fsr3_runtime_cvar_attempts);
            m_ue51_fsr3_runtime_cvars_done = true;
        }
    }

    if (!m_aphelion_framegen_runtime_cvars_done) {
        ++m_aphelion_framegen_runtime_cvar_attempts;

        if (force_aphelion_framegen_runtime_cvars_once(m_aphelion_framegen_runtime_cvar_attempts)) {
            m_aphelion_framegen_runtime_cvars_done = true;
        } else if (m_aphelion_framegen_runtime_cvar_attempts >= 600) {
            SPDLOG_WARN("[Aphelion][DX12] frame-generation cvars were not found after {} attempts; giving up", m_aphelion_framegen_runtime_cvar_attempts);
            m_aphelion_framegen_runtime_cvars_done = true;
        }
    }

    if (!m_windrose_shadow_runtime_cvars_done) {
        ++m_windrose_shadow_runtime_cvar_attempts;

        if (force_windrose_shadow_runtime_cvars_once(m_windrose_shadow_runtime_cvar_attempts)) {
            m_windrose_shadow_runtime_cvars_done = true;
        } else if (m_windrose_shadow_runtime_cvar_attempts >= 600) {
            SPDLOG_WARN("[Windrose][UE5.6][ShadowCrash] cvars were not found after {} attempts; giving up", m_windrose_shadow_runtime_cvar_attempts);
            m_windrose_shadow_runtime_cvars_done = true;
        }
    }
}

void CVarManager::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    m_cvar_ui_open_this_frame = false;

    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (ImGui::TreeNode("CVars")) {
        m_cvar_ui_open_this_frame = true;
        ImGui::TextWrapped("Note: Any changes here will be frozen.");

        uint32_t frozen_cvars = 0;

        for (auto& cvar : m_all_cvars) {
            if (cvar->is_frozen()) {
                ++frozen_cvars;
            }
        }

        ImGui::TextWrapped("Frozen CVars: %i", frozen_cvars);

        ImGui::Checkbox("Display Console", &m_wants_display_console);
        
        if (!m_native_console_spawned) {
            if (ImGui::Button("Spawn Native Console")) {
                spawn_console();
            }
        }

        if (ImGui::Button("Dump All CVars")) {
            GameThreadWorker::get().enqueue([this]() {
                dump_commands();
            });
        }

        ImGui::SameLine();

        if (ImGui::Button("Clear Frozen CVars")) {
            for (auto& cvar : m_all_cvars) {
                cvar->unfreeze();
            }

            refresh_frozen_cvar_state();

            const auto cvars_txt = Framework::get_persistent_dir(cvars_standard_txt_name.data());

            try {
                if (std::filesystem::exists(cvars_txt)) {
                    std::filesystem::remove(cvars_txt);
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to remove {}: {}", cvars_standard_txt_name.data(), e.what());
            }

            const auto cvars_data_txt = Framework::get_persistent_dir(cvars_data_txt_name.data());

            try {
                if (std::filesystem::exists(cvars_data_txt)) {
                    std::filesystem::remove(cvars_data_txt);
                }
            } catch (const std::exception& e) {
                spdlog::error("Failed to remove {}: {}", cvars_data_txt_name.data(), e.what());
            }
        }
        
        for (auto& cvar : m_displayed_cvars) {
            cvar->draw_ui();
        }

        refresh_frozen_cvar_state();

        ImGui::TreePop();
    }
}

void CVarManager::on_frame() {
    if (!g_framework->is_drawing_ui()) {
        m_cvar_ui_open_this_frame = false;
    }

    if (m_wants_display_console) {
        display_console();
    }
}

void CVarManager::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    const auto cvars_standard_txt = Framework::get_persistent_dir(cvars_standard_txt_name.data());
    utility::Config standard_cfg{};

    if (std::filesystem::exists(cvars_standard_txt)) {
        spdlog::info("[CVarManager] Loading {}...", cvars_standard_txt_name.data());
        standard_cfg.load(cvars_standard_txt.string());
    }

    const auto cvars_data_txt = Framework::get_persistent_dir(cvars_data_txt_name.data());
    utility::Config data_cfg{};

    if (std::filesystem::exists(cvars_data_txt)) {
        spdlog::info("[CVarManager] Loading {}...", cvars_data_txt_name.data());
        data_cfg.load(cvars_data_txt.string());
    }

    for (auto& cvar : m_all_cvars) {
        if (dynamic_cast<CVarStandard*>(cvar.get()) != nullptr) {
            cvar->load_from_config(standard_cfg, set_defaults);
        } else {
            cvar->load_from_config(data_cfg, set_defaults);
        }
    }

    refresh_frozen_cvar_state();
    m_needs_full_refresh = true;

    // TODO: Add arbitrary cvars from the other configs the user can add.

    // calling UEngine::exec here causes a crash, defer to on_pre_engine_tick()
    if (!set_defaults) {
        m_should_execute_console_script = true;
    }
}

void CVarManager::dump_commands() {
    const auto console_manager = sdk::FConsoleManager::get();

    if (console_manager == nullptr) {
        return;
    }

    nlohmann::json json;

    for (auto obj : console_manager->get_console_objects()) {
        if (obj.value == nullptr || obj.key == nullptr || IsBadReadPtr(obj.key, sizeof(wchar_t))) {
            continue;
        }

        auto& entry = json[utility::narrow(obj.key)];
        
        entry["description"] = "";
        //entry["address"] = (std::stringstream{} << std::hex << (uintptr_t)obj.value).str();
        //entry["vtable"] = (std::stringstream{} << std::hex << *(uintptr_t*)obj.value).str();

        bool is_command = false;

        try {
            is_command = obj.value->AsCommand() != nullptr;
            if (is_command) {
                entry["command"] = true;
            } else {
                entry["value"] = ((sdk::IConsoleVariable*)obj.value)->GetFloat();
            }
        } catch(...) {
            SPDLOG_WARN("Failed to check if CVar is a command: {}", utility::narrow(obj.key));
        }

        const auto help_string = obj.value->GetHelp();

        if (help_string != nullptr && !IsBadReadPtr(help_string, sizeof(wchar_t))) {
            try {
                SPDLOG_INFO("Found CVar: {} {}", utility::narrow(obj.key), utility::narrow(help_string));
                entry["description"] = utility::narrow(help_string);
            } catch(...) {

            }
        }
        
        SPDLOG_INFO("Found CVar: {}", utility::narrow(obj.key));
    }

    const auto persistent_dir = g_framework->get_persistent_dir();

    // Dump all CVars to a JSON file.
    std::ofstream file(persistent_dir / "cvardump.json");

    if (file.is_open()) {
        file << json.dump(4);
        file.close();

        SPDLOG_INFO("Dumped CVars to {}", (persistent_dir / "cvardump.json").string());
    }
}

// Use ImGui to display a homebrew console.
void CVarManager::display_console() {
    if (!g_framework->is_drawing_ui()) {
        return;
    }

    bool open = true;

    ImGui::SetNextWindowSize(ImVec2(800, 512), ImGuiCond_::ImGuiCond_Once);
    if (ImGui::Begin("UEVRConsole", &open)) {
        const auto console_manager = sdk::FConsoleManager::get();

        if (console_manager == nullptr) {
            ImGui::TextWrapped("Failed to get FConsoleManager.");
            ImGui::End();
            return;
        }


        ImGui::TextWrapped("Note: This is a homebrew console. It is not the same as the in-game console.");

        ImGui::Separator();

        ImGui::Text("> ");
        ImGui::SameLine();

        ImGui::PushItemWidth(-1);

        std::scoped_lock _{m_console.autocomplete_mutex};

        // Do a preliminary parse of the input buffer to see if we can autocomplete.
        {
            const auto entire_command = std::string_view{ m_console.input_buffer.data() };

            if (entire_command != m_console.last_parsed_buffer) {
                std::vector<std::string> args{};

                // Use getline
                std::stringstream ss{ entire_command.data() };
                while (ss.good()) {
                    std::string arg{};
                    std::getline(ss, arg, ' ');
                    args.push_back(arg);
                }

                if (!args.empty()) {
                    GameThreadWorker::get().enqueue([console_manager, args, this]() {
                        std::scoped_lock _{m_console.autocomplete_mutex};
                        m_console.autocomplete.clear();

                        const auto possible_commands = console_manager->fuzzy_find(utility::widen(args[0]));

                        for (const auto& command : possible_commands) {
                            std::string value = "Command";
                            std::string description = "";

                            try {
                                if (command.value->AsCommand() == nullptr) {
                                    value = std::format("{}", ((sdk::IConsoleVariable*)command.value)->GetFloat());
                                }
                            } catch(...) {
                                value = "Failed to get value.";
                            }

                            try {
                                const auto help_string = command.value->GetHelp();

                                if (help_string != nullptr && !IsBadReadPtr(help_string, sizeof(wchar_t))) {
                                    description = utility::narrow(help_string);
                                }
                            } catch(...) {
                                description = "Failed to get description.";
                            }

                            m_console.autocomplete.emplace_back(AutoComplete{
                                command.value, 
                                utility::narrow(command.key),
                                value,
                                description
                            });
                        }
                    });
                }

                m_console.last_parsed_buffer = entire_command;
            }
        }

        if (ImGui::InputText("##UEVRConsoleInput", m_console.input_buffer.data(), m_console.input_buffer.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
            m_console.input_buffer[m_console.input_buffer.size() - 1] = '\0';

            if (m_console.input_buffer[0] != '\0') {
                const auto entire_command = std::string_view{ m_console.input_buffer.data() };

                // Split the command into the arguments via ' ' (space).
                std::vector<std::string> args{};

                // Use getline
                std::stringstream ss{ entire_command.data() };
                while (ss.good()) {
                    std::string arg{};
                    std::getline(ss, arg, ' ');
                    args.push_back(arg);
                }

                // Execute the command.
                if (args.size() >= 2) {
                    auto object = console_manager->find(utility::widen(args[0]));
                    const auto is_command = object != nullptr && object->AsCommand() != nullptr;

                    if (object != nullptr && !is_command) {
                        auto var = (sdk::IConsoleVariable*)object;
                        
                        GameThreadWorker::get().enqueue([var, value = utility::widen(args[1])]() {
                            var->Set(value.c_str());
                        });
                    } else if (object != nullptr && is_command) {
                        auto command = (sdk::IConsoleCommand*)object;

                        std::vector<std::wstring> widened_args{};
                        for (auto i = 1; i < args.size(); ++i) {
                            widened_args.push_back(utility::widen(args[i]));
                        }

                        GameThreadWorker::get().enqueue([command, widened_args]() {
                            command->Execute(widened_args);
                        });
                    } else if (object == nullptr) {
                        // Try UEngine::Exec
                        std::string entire_command_str{entire_command.data()};
                        GameThreadWorker::get().enqueue([entire_command_str]() {
                            auto engine = sdk::UGameEngine::get();
                            if (engine != nullptr) {
                                engine->exec(utility::widen(entire_command_str).data());
                            }
                        });
                    }
                }

                m_console.history.push_back(m_console.input_buffer.data());
                m_console.history_index = m_console.history.size();

                m_console.input_buffer.fill('\0');
            }
        }

        // Display autocomplete
        if (!m_console.autocomplete.empty()) {
            // Create a table of all the possible commands.
            if (ImGui::BeginTable("##UEVRAutocomplete", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
                ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 300.0f);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (const auto& command : m_console.autocomplete) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(command.name.c_str());

                    if (ImGui::IsItemClicked()) {
                        // Copy the command to the input buffer.
                        std::copy(command.name.begin(), command.name.end(), m_console.input_buffer.begin());
                        m_console.input_buffer[command.name.size()] = '\0';
                    }

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(command.current_value.c_str());

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextWrapped(command.description.c_str());
                }

                ImGui::EndTable();
            }
        }

        ImGui::End();
    }
}

std::string CVarManager::CVar::get_key_name() {
    ZoneScopedN(__FUNCTION__);

    return std::format("{}_{}", utility::narrow(m_module), utility::narrow(m_name));
}

int CVarManager::CVar::clamp_int_value(int value) const {
    return std::clamp(value, m_min_int_value, effective_max_int_value());
}

float CVarManager::CVar::clamp_float_value(float value) const {
    if (!std::isfinite(value)) {
        return m_min_float_value;
    }

    return std::clamp(value, m_min_float_value, m_max_float_value);
}

int CVarManager::CVar::effective_max_int_value() const {
    if (m_name == L"r.PostProcessing.PropagateAlpha" && !propagate_alpha_allows_tonemapper_value()) {
        return std::min(m_max_int_value, 1);
    }

    return m_max_int_value;
}

void CVarManager::CVar::load_internal(const std::string& filename, bool set_defaults) try {
    ZoneScopedN(__FUNCTION__);

    spdlog::info("[CVarManager] Loading {}...", filename);

    const auto cvars_txt = Framework::get_persistent_dir(filename);

    if (!std::filesystem::exists(cvars_txt)) {
        return;
    }

    auto cfg = utility::Config{cvars_txt.string()};
    auto value = cfg.get(get_key_name());

    if (!value) {
        // No need to freeze.
        return;
    }
    
    switch (m_type) {
    case Type::BOOL:
    case Type::INT:
        try {
            m_frozen_int_value = clamp_int_value(*cfg.get<int>(get_key_name()));
        } catch(...) {
            m_frozen_int_value = clamp_int_value((int)*cfg.get<float>(get_key_name()));
        }
        break;
    case Type::FLOAT:
        try {
            m_frozen_float_value = clamp_float_value(*cfg.get<float>(get_key_name()));
        } catch(...) {
            m_frozen_float_value = clamp_float_value((float)*cfg.get<int>(get_key_name()));
        }
        break;
    }

    m_frozen = true;
} catch(const std::exception& e) {
    spdlog::error("Failed to load {}: {}", filename, e.what());
}

void CVarManager::CVar::load_from_config_internal(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    const auto value = cfg.get(get_key_name());

    if (!value) {
        return;
    }

    switch (m_type) {
    case Type::BOOL:
    case Type::INT:
        try {
            m_frozen_int_value = clamp_int_value(*cfg.get<int>(get_key_name()));
        } catch (...) {
            m_frozen_int_value = clamp_int_value((int)*cfg.get<float>(get_key_name()));
        }
        break;
    case Type::FLOAT:
        try {
            m_frozen_float_value = clamp_float_value(*cfg.get<float>(get_key_name()));
        } catch (...) {
            m_frozen_float_value = clamp_float_value((float)*cfg.get<int>(get_key_name()));
        }
        break;
    }

    m_frozen = true;
}

void CVarManager::CVar::save_internal(const std::string& filename) try {
    ZoneScopedN(__FUNCTION__);
    
    spdlog::info("[CVarManager] Saving {}...", filename);

    const auto cvars_txt = Framework::get_persistent_dir(filename);

    auto cfg = utility::Config{cvars_txt.string()};

    switch (m_type) {
    case Type::BOOL:
    case Type::INT:
        m_frozen_int_value = clamp_int_value(m_frozen_int_value);
        cfg.set<int>(get_key_name(), m_frozen_int_value);
        break;
    case Type::FLOAT:
        m_frozen_float_value = clamp_float_value(m_frozen_float_value);
        cfg.set<float>(get_key_name(), m_frozen_float_value);
        break;
    };

    cfg.save(cvars_txt.string());
    m_frozen = true;
} catch (const std::exception& e) {
    spdlog::error("Failed to save {}: {}", filename, e.what());
}

void CVarManager::CVarStandard::load(bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_internal(cvars_standard_txt_name.data(), set_defaults);
}

void CVarManager::CVarStandard::load_from_config(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_from_config_internal(cfg, set_defaults);
}

void CVarManager::CVarStandard::save() {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        // CVar not found, don't save.
        return;
    }

    auto cvar = *m_cvar;

    switch (m_type) {
    case Type::BOOL:
        m_frozen_int_value = cvar->GetInt();
        break;
    case Type::INT:
        m_frozen_int_value = cvar->GetInt();
        break;
    case Type::FLOAT:
        m_frozen_float_value = cvar->GetFloat();
        break;
    default:
        break;
    }

    save_internal(cvars_standard_txt_name.data());
}

void CVarManager::CVarStandard::freeze() {
    ZoneScopedN(__FUNCTION__);

    if (!m_frozen || m_setter_unavailable) {
        return;
    }

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        return;
    }

    if (!m_ever_frozen) {
        m_ever_frozen = true;
        SPDLOG_INFO("[CVarManager] (Standard) First time freezing \"{}\"...", utility::narrow(m_name));
    }

    switch(m_type) {
    case Type::BOOL:
        // Limiting the amount of times Set gets called with string conversions.
        if ((*m_cvar)->GetInt() != m_frozen_int_value) {
            if (!(*m_cvar)->Set(std::to_wstring(m_frozen_int_value).c_str())) {
                m_setter_unavailable = true;
                SPDLOG_WARN("[CVarManager] (Standard) Disabling freeze enforcement for \"{}\" because its setter is unavailable", utility::narrow(m_name));
            }
        }
        break;
    case Type::INT:
        if ((*m_cvar)->GetInt() != m_frozen_int_value) {
            if (!(*m_cvar)->Set(std::to_wstring(m_frozen_int_value).c_str())) {
                m_setter_unavailable = true;
                SPDLOG_WARN("[CVarManager] (Standard) Disabling freeze enforcement for \"{}\" because its setter is unavailable", utility::narrow(m_name));
            }
        }
        break;
    case Type::FLOAT:
        if ((*m_cvar)->GetFloat() != m_frozen_float_value) {
            if (!(*m_cvar)->Set(std::to_wstring(m_frozen_float_value).c_str())) {
                m_setter_unavailable = true;
                SPDLOG_WARN("[CVarManager] (Standard) Disabling freeze enforcement for \"{}\" because its setter is unavailable", utility::narrow(m_name));
            }
        }
        break;
    default:
        break;
    };
}

void CVarManager::CVarStandard::update() {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr) {
        m_cvar = sdk::find_cvar_cached(m_module, m_name);
    }
}

void CVarManager::CVarStandard::draw_ui() try {
    ZoneScopedN(__FUNCTION__);

    if (m_cvar == nullptr || *m_cvar == nullptr) {
        ImGui::TextWrapped("Failed to find cvar: %s", utility::narrow(m_name).c_str());
        return;
    }

    auto cvar = *m_cvar;
    const auto narrow_name = utility::narrow(m_name);
    
    switch (m_type) {
    case Type::BOOL: {
        auto value = (bool)(m_frozen ? m_frozen_int_value : cvar->GetInt());

        if (ImGui::Checkbox(narrow_name.c_str(), &value)) {
            m_frozen_int_value = clamp_int_value((int)value);
            m_setter_unavailable = false;
            CVarManager::record_global_change(m_name, std::to_wstring(m_frozen_int_value), "standard_ui");
            save_internal(cvars_standard_txt_name.data());

            GameThreadWorker::get().enqueue([sft = std::static_pointer_cast<CVarStandard>(shared_from_this()), cvar, value]() {
                try {
                    if (cvar->Set(std::to_wstring((int)value).c_str())) {
                        sft->m_setter_unavailable = false;
                    } else {
                        sft->m_setter_unavailable = true;
                        spdlog::warn("Setter unavailable for cvar: {}", utility::narrow(sft->get_name()));
                    }
                } catch (...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    case Type::INT: {
        auto value = m_frozen ? m_frozen_int_value : cvar->GetInt();

        if (ImGui::SliderInt(narrow_name.c_str(), &value, m_min_int_value, effective_max_int_value())) {
            value = clamp_int_value(value);
            m_frozen_int_value = value;
            m_setter_unavailable = false;
            CVarManager::record_global_change(m_name, std::to_wstring(value), "standard_ui");
            save_internal(cvars_standard_txt_name.data());

            GameThreadWorker::get().enqueue([sft = std::static_pointer_cast<CVarStandard>(shared_from_this()), cvar, value]() {
                try {
                    if (cvar->Set(std::to_wstring(value).c_str())) {
                        sft->m_setter_unavailable = false;
                    } else {
                        sft->m_setter_unavailable = true;
                        spdlog::warn("Setter unavailable for cvar: {}", utility::narrow(sft->get_name()));
                    }
                } catch(...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    case Type::FLOAT: {
        auto value = m_frozen ? m_frozen_float_value : cvar->GetFloat();

        if (ImGui::SliderFloat(narrow_name.c_str(), &value, m_min_float_value, m_max_float_value)) {
            value = clamp_float_value(value);
            m_frozen_float_value = value;
            m_setter_unavailable = false;
            CVarManager::record_global_change(m_name, std::to_wstring(value), "standard_ui");
            save_internal(cvars_standard_txt_name.data());

            GameThreadWorker::get().enqueue([sft = std::static_pointer_cast<CVarStandard>(shared_from_this()), cvar, value]() {
                try {
                    if (cvar->Set(std::to_wstring(value).c_str())) {
                        sft->m_setter_unavailable = false;
                    } else {
                        sft->m_setter_unavailable = true;
                        spdlog::warn("Setter unavailable for cvar: {}", utility::narrow(sft->get_name()));
                    }
                } catch(...) {
                    spdlog::error("Failed to set cvar: {}", utility::narrow(sft->get_name()));
                }
            });
        }
        break;
    }
    default:
        ImGui::TextWrapped("Unimplemented cvar type: %s", utility::narrow(m_name).c_str());
        break;
    };
} catch(...) {
    ImGui::TextWrapped("Failed to read cvar: %s", utility::narrow(m_name).c_str());
}

void CVarManager::CVarData::load(bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_internal(cvars_data_txt_name.data(), set_defaults);
}

void CVarManager::CVarData::load_from_config(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    load_from_config_internal(cfg, set_defaults);
}

void CVarManager::CVarData::save() {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        return;
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        return;
    }

    switch (m_type) {
    case Type::BOOL:
        m_frozen_int_value = cvar_int->get();
        break;
    case Type::INT:
        m_frozen_int_value = cvar_int->get();
        break;
    case Type::FLOAT:
        m_frozen_float_value = cvar_float->get();
        break;
    default:
        break;
    };

    save_internal(cvars_data_txt_name.data());
}

void CVarManager::CVarData::freeze() {
    ZoneScopedN(__FUNCTION__);

    if (!m_frozen) {
        return;
    }

    if (!m_cvar_data) {
        return;
    }

    if (!m_ever_frozen) {
        m_ever_frozen = true;
        SPDLOG_INFO("[CVarManager] (Data) First time freezing \"{}\"...", utility::narrow(m_name));
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        return;
    }

    switch (m_type) {
    case Type::BOOL:
        cvar_int->set(m_frozen_int_value);
        break;
    case Type::INT:
        cvar_int->set(m_frozen_int_value);
        break;
    case Type::FLOAT:
        cvar_float->set(m_frozen_float_value);
        break;
    default:
        break;
    };
}

void CVarManager::CVarData::update() {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        m_cvar_data = sdk::find_cvar_data_cached(m_module, m_name);
    }
}

void CVarManager::CVarData::draw_ui() try {
    ZoneScopedN(__FUNCTION__);

    if (!m_cvar_data) {
        ImGui::TextWrapped("Failed to find cvar data: %s", utility::narrow(m_name).c_str());
        return;
    }

    // Points to the same thing, just different data internally.
    auto cvar_int = m_cvar_data->get<int>();
    auto cvar_float = m_cvar_data->get<float>();

    if (cvar_int == nullptr) {
        ImGui::TextWrapped("Failed to read cvar data: %s", utility::narrow(m_name).c_str());
        return;
    }

    const auto narrow_name = utility::narrow(m_name);

    switch (m_type) {
    case Type::BOOL: {
        auto value = (bool)cvar_int->get();

        if (ImGui::Checkbox(narrow_name.c_str(), &value)) {
            cvar_int->set((int)value); // no need to run on game thread, direct access
            CVarManager::record_global_change(m_name, std::to_wstring((int)value), "data_ui");
            this->save();
        }
        break;
    }
    case Type::INT: {
        auto value = cvar_int->get();

        if (ImGui::SliderInt(narrow_name.c_str(), &value, m_min_int_value, m_max_int_value)) {
            cvar_int->set(value); // no need to run on game thread, direct access
            CVarManager::record_global_change(m_name, std::to_wstring(value), "data_ui");
            this->save();
        }
        break;
    }
    case Type::FLOAT: {
        auto value = cvar_float->get();

        if (ImGui::SliderFloat(narrow_name.c_str(), &value, m_min_float_value, m_max_float_value)) {
            cvar_float->set(value); // no need to run on game thread, direct access
            CVarManager::record_global_change(m_name, std::to_wstring(value), "data_ui");
            this->save();
        }
        break;
    }
    default:
        ImGui::TextWrapped("Unimplemented cvar type: %s", narrow_name.c_str());
        break;
    }
} catch (...) {
    ImGui::TextWrapped("Failed to read cvar data: %s", utility::narrow(m_name).c_str());
}

static inline void trim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));

    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
}

void CVarManager::execute_console_script(sdk::UGameEngine* engine, const std::string& filename) {
    ZoneScopedN(__FUNCTION__);

    if (engine == nullptr) {
        spdlog::error("[execute_console_script] engine is null");
        return;
    }

    spdlog::info("[execute_console_script] Loading {}...", filename);

    const auto cscript_txt = Framework::get_persistent_dir(filename);

    if (!std::filesystem::exists(cscript_txt)) {
        return;
    }

    std::ifstream cscript_file(utility::widen(cscript_txt.string()));

    if (!cscript_file) {
        spdlog::error("[execute_console_script] Failed to open file {}...", filename);
        return;
    }

    for (std::string line{}; getline(cscript_file, line); ) {
        trim(line);

        // handle comments
        if (line.starts_with('#') || line.starts_with(';')) {
            continue;
        }

        if (line.contains('#')) {
            line = line.substr(0, line.find_first_of('#'));
            trim(line);
        }

        if (line.contains(';')) {
            line = line.substr(0, line.find_first_of(';'));
            trim(line);
        }

        if (line.length() == 0) {
            continue;
        }

        spdlog::debug("[execute_console_script] Attempting to execute \"{}\"", line);
        CVarManager::record_global_command(line, "console_script");
        engine->exec(utility::widen(line));
    }

    spdlog::debug("[execute_console_script] done");
}
