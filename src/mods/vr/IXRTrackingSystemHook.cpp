#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>

#include <bdshemu.h>
#include <spdlog/spdlog.h>

#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include <utility/Emulation.hpp>
#include <utility/Patch.hpp>

#include "utility/Logging.hpp"

#include <sdk/Utility.hpp>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/UObjectBase.hpp>
#include <sdk/FProperty.hpp>
#include <sdk/ScriptRotator.hpp>
#include <sdk/ScriptVector.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/APawn.hpp>

#include "../VR.hpp"

#include <sdk/vtables/IXRTrackingSystemVTables.hpp>
#include <sdk/structures/FXRMotionControllerData.hpp>
#include <sdk/structures/FXRHMDData.hpp>
#include <sdk/UHeadMountedDisplayFunctionLibrary.hpp>
#include <sdk/UFunction.hpp>

#include "IXRTrackingSystemHook.hpp"

namespace {
bool is_deadzone_ue56_executable() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path || exe_path->find(L"DeadzoneSteam-Win64-Shipping") == std::wstring::npos) {
            return false;
        }

        const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
        const auto file_version = sdk::get_file_version_info();

        return str_version.starts_with("5.6") || file_version.dwFileVersionMS == 0x00050006;
    }();

    return result;
}

bool is_daysgone_executable() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());

        if (!exe_path) {
            return false;
        }

        const auto separator = exe_path->find_last_of(L"\\/");
        const auto filename = separator == std::wstring::npos ? *exe_path : exe_path->substr(separator + 1);
        return _wcsicmp(filename.c_str(), L"DaysGone.exe") == 0;
    }();

    return result;
}

bool is_daysgone_native_aim_requested() {
    return is_daysgone_executable() && VR::get()->is_any_aim_method_active();
}

bool is_ue4_14_through_4_17() {
    static const bool result = []() {
        if (const auto found_version = sdk::search_for_version(utility::get_executable())) {
            const auto version = utility::narrow(*found_version);

            try {
                const auto separator = version.find('.');

                if (separator != std::string::npos) {
                    const auto major = std::stoi(version.substr(0, separator));
                    const auto minor = std::stoi(version.substr(separator + 1));
                    return major == 4 && minor >= 14 && minor <= 17;
                }
            } catch (...) {
                // Fall through to the executable's file version.
            }
        }

        const auto file_version = sdk::get_file_version_info();
        const auto major = HIWORD(file_version.dwFileVersionMS);
        const auto minor = LOWORD(file_version.dwFileVersionMS);
        return major == 4 && minor >= 14 && minor <= 17;
    }();

    return result;
}

bool is_guarded_legacy_aim_path() {
    return is_ue4_14_through_4_17();
}

bool is_live_legacy_aim_object(sdk::UObjectBase* object) try {
    if (!is_guarded_legacy_aim_path() || object == nullptr ||
        IsBadReadPtr(object, sdk::UObjectBase::get_class_size())) {
        return false;
    }

    const auto object_array = sdk::FUObjectArray::get();

    if (object_array == nullptr) {
        return false;
    }

    const auto index = object->get_internal_index();

    if (index < 0 || index >= object_array->get_object_count()) {
        return false;
    }

    const auto item = object_array->get_object(index);

    if (item == nullptr || item->get_object() != object) {
        return false;
    }

    const auto object_class = object->get_class();
    return object_class != nullptr && !IsBadReadPtr(object_class, sdk::UObjectBase::get_class_size());
} catch (...) {
    return false;
}

bool is_legacy_aim_reflection_ready() try {
    if (!is_guarded_legacy_aim_path()) {
        return true;
    }

    static bool ready = false;
    static auto next_retry = std::chrono::steady_clock::time_point{};

    if (ready) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();

    if (now < next_retry) {
        return false;
    }

    next_retry = now + std::chrono::seconds(1);

    if (sdk::FUObjectArray::get() == nullptr) {
        return false;
    }

    const auto controller_class = sdk::AController::static_class();
    const auto rotator = sdk::ScriptRotator::static_struct();

    if (controller_class == nullptr || rotator == nullptr ||
        rotator->get_struct_size() != sizeof(glm::vec3)) {
        return false;
    }

    const auto control_rotation = controller_class->find_property(L"ControlRotation");
    ready = control_rotation != nullptr;

    if (ready) {
        SPDLOG_INFO("[UE4.14-4.17][Aim] Validated reflected ControlRotation and 12-byte FRotator layout for direct aim");
    }

    return ready;
} catch (...) {
    return false;
}

bool is_payday3_aim_guard_enabled();

bool is_direct_aim_compatibility_requested() {
    if (is_daysgone_executable()) {
        return false;
    }

    if (is_deadzone_ue56_executable()) {
        return true;
    }

    if (is_payday3_aim_guard_enabled()) {
        return true;
    }

    if (is_ue4_14_through_4_17()) {
        return true;
    }

    return VR::get()->is_direct_aim_compatibility_enabled();
}

bool is_direct_aim_compatibility_active() {
    if (is_daysgone_executable()) {
        return false;
    }

    if (!is_direct_aim_compatibility_requested()) {
        return false;
    }

    auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return false;
    }

    if (vr->is_controller_camera_conflict_guard_active()) {
        return false;
    }

    if (is_deadzone_ue56_executable()) {
        // Deadzone's UE5.6 UObject/FName path is unsafe from the direct
        // fallback tick path. Let the normal ProcessViewRotation hook drive
        // HMD/controller aim instead of scanning or resolving UObject names.
        return false;
    }

    if (is_payday3_aim_guard_enabled()) {
        // PAYDAY3 crashes inside its next UGameViewportClient::Draw when UEVR
        // mutates the camera-manager ProcessViewRotation output directly.
        // Route HMD/controller aim through the later reflected controller path.
        return vr->is_headlocked_aim_enabled() || (vr->is_controller_aim_enabled() && vr->is_using_controllers());
    }

    if (is_ue4_14_through_4_17()) {
        // UE4.14-4.17 exposes only the legacy IHeadMountedDisplay interface.
        // Keep its incomplete fake-HMD vtable out of the aim path even while a
        // configured motion controller is still becoming available.
        return vr->is_headlocked_aim_enabled() || vr->is_controller_aim_enabled();
    }

    if (vr->is_headlocked_aim_enabled()) {
        return true;
    }

    return vr->is_controller_aim_enabled() && vr->is_using_controllers();
}

bool is_payday3_aim_guard_enabled() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"PAYDAY3-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

sdk::APlayerController* resolve_player_controller_for_aim(sdk::UEngine* engine, sdk::UWorld* world) {
    if (engine != nullptr) {
        if (const auto local_player = reinterpret_cast<sdk::UObject*>(engine->get_localplayer(0)); local_player != nullptr) {
            if (const auto data = local_player->get_property_data(L"PlayerController"); data != nullptr && !IsBadReadPtr(data, sizeof(void*))) {
                if (const auto controller = *(sdk::APlayerController**)data; controller != nullptr) {
                    return controller;
                }
            }
        }
    }

    if (is_payday3_aim_guard_enabled()) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[PAYDAY3][Aim] PlayerController unavailable through LocalPlayer reflection; skipping GameplayStatics fallback");
        return nullptr;
    }
    if (is_ue4_14_through_4_17()) {
        SPDLOG_WARNING_EVERY_N_SEC(
            2,
            "[UE4.14-4.17][Aim] PlayerController unavailable through LocalPlayer reflection; skipping ProcessEvent fallback");
        return nullptr;
    }

    if (world == nullptr || sdk::UGameplayStatics::static_class() == nullptr) {
        return nullptr;
    }

    const auto gameplay = sdk::UGameplayStatics::get();
    return gameplay != nullptr ? gameplay->get_player_controller(world, 0) : nullptr;
}

sdk::APawn* resolve_acknowledged_pawn_for_aim(sdk::APlayerController* controller) {
    if (controller == nullptr) {
        return nullptr;
    }

    if (is_payday3_aim_guard_enabled() || is_ue4_14_through_4_17()) {
        const auto controller_obj = reinterpret_cast<sdk::UObject*>(controller);

        if (const auto data = controller_obj->get_property_data(L"AcknowledgedPawn"); data != nullptr && !IsBadReadPtr(data, sizeof(void*))) {
            return *(sdk::APawn**)data;
        }

        return nullptr;
    }

    return controller->get_acknowledged_pawn();
}

bool read_payday3_control_rotation_property(sdk::APlayerController* controller, glm::vec3& out) {
    if (!is_payday3_aim_guard_enabled() || controller == nullptr) {
        return false;
    }

    const auto controller_obj = reinterpret_cast<sdk::UObject*>(controller);
    const auto data = controller_obj->get_property_data(L"ControlRotation");

    if (data == nullptr || IsBadReadPtr(data, sizeof(glm::vec3))) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[PAYDAY3][Aim] ControlRotation property is unavailable for direct read");
        return false;
    }

    out = *(glm::vec3*)data;
    return true;
}

bool write_payday3_control_rotation_property(sdk::APlayerController* controller, const glm::vec3& value) {
    if (!is_payday3_aim_guard_enabled() || controller == nullptr) {
        return false;
    }

    const auto controller_obj = reinterpret_cast<sdk::UObject*>(controller);
    const auto data = controller_obj->get_property_data(L"ControlRotation");

    if (data == nullptr || IsBadWritePtr(data, sizeof(glm::vec3))) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[PAYDAY3][Aim] ControlRotation property is unavailable for direct write");
        return false;
    }

    *(glm::vec3*)data = value;
    return true;
}

}

detail::IXRTrackingSystemVT& get_tracking_system_vtable(std::optional<std::string> version_override = std::nullopt) {
    const auto str_version = version_override.has_value() ? version_override.value() : utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    auto version = sdk::get_file_version_info();

    if (str_version != "0.00") {
        SPDLOG_INFO("Found version {} from executable", str_version);
        version.dwFileVersionMS = 0;
    } else {
        SPDLOG_INFO("Found version {}.{} from executable (disk version)", HIWORD(version.dwFileVersionMS), LOWORD(version.dwFileVersionMS));
    }

    if (version.dwFileVersionMS == 0x50008 || str_version.starts_with("5.8")) {
        return ue5_8::IXRTrackingSystemVT::get();
    }

    if (version.dwFileVersionMS == 0x50007 || str_version.starts_with("5.7")) {
        return ue5_7::IXRTrackingSystemVT::get();
    }

    if (version.dwFileVersionMS == 0x50006 || str_version.starts_with("5.6")) {
        return ue5_6::IXRTrackingSystemVT::get();
    }

    if (version.dwFileVersionMS == 0x50005 || str_version.starts_with("5.5")) {
        return ue5_5::IXRTrackingSystemVT::get();
    }

    if (version.dwFileVersionMS == 0x50004 || str_version.starts_with("5.4")) {
        return ue5_4::IXRTrackingSystemVT::get();
    }

    // >= 5.3
    if (version.dwFileVersionMS == 0x50003 || str_version.starts_with("5.3")) {
        return ue5_3::IXRTrackingSystemVT::get();
    }

    // TODO: actually dump 5.2
    // >= 5.2
    if (version.dwFileVersionMS == 0x50002 || str_version.starts_with("5.2")) {
        return ue5_1::IXRTrackingSystemVT::get();
    }

    // >= 5.1
    if (version.dwFileVersionMS == 0x50001 || str_version.starts_with("5.1")) {
        return ue5_1::IXRTrackingSystemVT::get();
    }

    // >= 5.0
    if (version.dwFileVersionMS == 0x50000 || str_version.starts_with("5.0")) {
        return ue5_00::IXRTrackingSystemVT::get();
    }

    // 4.27
    if (version.dwFileVersionMS == 0x4001B || str_version.starts_with("4.27")) {
        return ue4_27::IXRTrackingSystemVT::get();
    }

    // 4.26
    if (version.dwFileVersionMS == 0x4001A || str_version.starts_with("4.26")) {
        return ue4_26::IXRTrackingSystemVT::get();
    }
    
    // 4.25
    if (version.dwFileVersionMS == 0x40019 || str_version.starts_with("4.25")) {
        return ue4_25::IXRTrackingSystemVT::get();
    }

    // 4.24
    if (version.dwFileVersionMS == 0x40018 || str_version.starts_with("4.24")) {
        return ue4_24::IXRTrackingSystemVT::get();
    }

    // 4.23
    if (version.dwFileVersionMS == 0x40017 || str_version.starts_with("4.23")) {
        return ue4_23::IXRTrackingSystemVT::get();
    }

    // 4.22
    if (version.dwFileVersionMS == 0x40016 || str_version.starts_with("4.22")) {
        return ue4_22::IXRTrackingSystemVT::get();
    }

    // 4.21
    if (version.dwFileVersionMS == 0x40015 || str_version.starts_with("4.21")) {
        return ue4_21::IXRTrackingSystemVT::get();
    }

    // 4.20
    if (version.dwFileVersionMS == 0x40014 || str_version.starts_with("4.20")) {
        return ue4_20::IXRTrackingSystemVT::get();
    }

    // 4.19
    if (version.dwFileVersionMS == 0x40013 || str_version.starts_with("4.19")) {
        return ue4_19::IXRTrackingSystemVT::get();
    }

    // 4.18
    if (version.dwFileVersionMS == 0x40012 || str_version.starts_with("4.18")) {
        return ue4_18::IXRTrackingSystemVT::get();
    }

    // versions lower than 4.18 do not have IXRTrackingSystem
    return detail::IXRTrackingSystemVT::get();
}


detail::IXRCameraVT& get_camera_vtable(std::optional<std::string> version_override = std::nullopt) {
    const auto str_version = version_override.has_value() ? version_override.value() : utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    auto version = sdk::get_file_version_info();

    if (str_version != "0.00") {
        version.dwFileVersionMS = 0;
    }

    if (version.dwFileVersionMS == 0x50008 || str_version.starts_with("5.8")) {
        return ue5_8::IXRCameraVT::get();
    }

    if (version.dwFileVersionMS == 0x50007 || str_version.starts_with("5.7")) {
        return ue5_7::IXRCameraVT::get();
    }

    if (version.dwFileVersionMS == 0x50006 || str_version.starts_with("5.6")) {
        return ue5_6::IXRCameraVT::get();
    }

    if (version.dwFileVersionMS == 0x50005 || str_version.starts_with("5.5")) {
        return ue5_5::IXRCameraVT::get();
    }

    if (version.dwFileVersionMS == 0x50004 || str_version.starts_with("5.4")) {
        return ue5_4::IXRCameraVT::get();
    }

    // TODO: actually dump 5.2
    if (version.dwFileVersionMS == 0x50003 || str_version.starts_with("5.3")) {
        return ue5_3::IXRCameraVT::get();
    }

    // >= 5.2
    if (version.dwFileVersionMS == 0x50002 || str_version.starts_with("5.2")) {
        return ue5_1::IXRCameraVT::get();
    }

    // >= 5.1
    if (version.dwFileVersionMS == 0x50001 || str_version.starts_with("5.1")) {
        return ue5_1::IXRCameraVT::get();
    }

    // >= 5.0
    if (version.dwFileVersionMS == 0x50000 || str_version.starts_with("5.0")) {
        return ue5_00::IXRCameraVT::get();
    }

    // 4.27
    if (version.dwFileVersionMS == 0x4001B || str_version.starts_with("4.27")) {
        return ue4_27::IXRCameraVT::get();
    }

    // 4.26
    if (version.dwFileVersionMS == 0x4001A || str_version.starts_with("4.26")) {
        return ue4_26::IXRCameraVT::get();
    }

    // 4.25
    if (version.dwFileVersionMS == 0x40019 || str_version.starts_with("4.25")) {
        return ue4_25::IXRCameraVT::get();
    }

    // 4.24
    if (version.dwFileVersionMS == 0x40018 || str_version.starts_with("4.24")) {
        return ue4_24::IXRCameraVT::get();
    }

    // 4.23
    if (version.dwFileVersionMS == 0x40017 || str_version.starts_with("4.23")) {
        return ue4_23::IXRCameraVT::get();
    }

    // 4.22
    if (version.dwFileVersionMS == 0x40016 || str_version.starts_with("4.22")) {
        return ue4_22::IXRCameraVT::get();
    }

    // 4.21
    if (version.dwFileVersionMS == 0x40015 || str_version.starts_with("4.21")) {
        return ue4_21::IXRCameraVT::get();
    }

    // 4.20
    if (version.dwFileVersionMS == 0x40014 || str_version.starts_with("4.20")) {
        return ue4_20::IXRCameraVT::get();
    }

    // 4.19
    if (version.dwFileVersionMS == 0x40013 || str_version.starts_with("4.19")) {
        return ue4_19::IXRCameraVT::get();
    }

    // 4.18
    if (version.dwFileVersionMS == 0x40012 || str_version.starts_with("4.18")) {
        return ue4_18::IXRCameraVT::get();
    }

    // Versions lower than 4.18 do not have IXRCamera
    return detail::IXRCameraVT::get();
}

detail::IHeadMountedDisplayVT& get_hmd_vtable(std::optional<std::string> version_override = std::nullopt) {
    const auto str_version = version_override.has_value() ? version_override.value() : utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    auto version = sdk::get_file_version_info();

    if (str_version != "0.00") {
        version.dwFileVersionMS = 0;
    }

    if (version.dwFileVersionMS == 0x50008 || str_version.starts_with("5.8")) {
        return ue5_8::IHeadMountedDisplayVT::get();
    }

    if (version.dwFileVersionMS == 0x50007 || str_version.starts_with("5.7")) {
        return ue5_7::IHeadMountedDisplayVT::get();
    }

    if (version.dwFileVersionMS == 0x50006 || str_version.starts_with("5.6")) {
        return ue5_6::IHeadMountedDisplayVT::get();
    }

    if (version.dwFileVersionMS == 0x50005 || str_version.starts_with("5.5")) {
        return ue5_5::IHeadMountedDisplayVT::get();
    }

    if (version.dwFileVersionMS == 0x50004 || str_version.starts_with("5.4")) {
        return ue5_4::IHeadMountedDisplayVT::get();
    }

    // 5.3
    if (version.dwFileVersionMS == 0x50003 || str_version.starts_with("5.3")) {
        return ue5_3::IHeadMountedDisplayVT::get();
    }

    // TODO: actually dump 5.2
    // >= 5.2
    if (version.dwFileVersionMS == 0x50002 || str_version.starts_with("5.2")) {
        return ue5_1::IHeadMountedDisplayVT::get();
    }

    // >= 5.1
    if (version.dwFileVersionMS == 0x50001 || str_version.starts_with("5.1")) {
        return ue5_1::IHeadMountedDisplayVT::get();
    }

    // >= 5.0
    if (version.dwFileVersionMS == 0x50000 || str_version.starts_with("5.0")) {
        return ue5_00::IHeadMountedDisplayVT::get();
    }

    // 4.27
    if (version.dwFileVersionMS == 0x4001B || str_version.starts_with("4.27")) {
        return ue4_27::IHeadMountedDisplayVT::get();
    }

    // 4.26
    if (version.dwFileVersionMS == 0x4001A || str_version.starts_with("4.26")) {
        return ue4_26::IHeadMountedDisplayVT::get();
    }

    // 4.25
    if (version.dwFileVersionMS == 0x40019 || str_version.starts_with("4.25")) {
        return ue4_25::IHeadMountedDisplayVT::get();
    }

    // 4.24
    if (version.dwFileVersionMS == 0x40018 || str_version.starts_with("4.24")) {
        return ue4_24::IHeadMountedDisplayVT::get();
    }

    // 4.23
    if (version.dwFileVersionMS == 0x40017 || str_version.starts_with("4.23")) {
        return ue4_23::IHeadMountedDisplayVT::get();
    }

    // 4.22
    if (version.dwFileVersionMS == 0x40016 || str_version.starts_with("4.22")) {
        return ue4_22::IHeadMountedDisplayVT::get();
    }

    // 4.21
    if (version.dwFileVersionMS == 0x40015 || str_version.starts_with("4.21")) {
        return ue4_21::IHeadMountedDisplayVT::get();
    }

    // 4.20
    if (version.dwFileVersionMS == 0x40014 || str_version.starts_with("4.20")) {
        return ue4_20::IHeadMountedDisplayVT::get();
    }

    // 4.19
    if (version.dwFileVersionMS == 0x40013 || str_version.starts_with("4.19")) {
        return ue4_19::IHeadMountedDisplayVT::get();
    }

    // 4.18
    if (version.dwFileVersionMS == 0x40012 || str_version.starts_with("4.18")) {
        return ue4_18::IHeadMountedDisplayVT::get();
    }

    // 4.17
    if (version.dwFileVersionMS == 0x40011 || str_version.starts_with("4.17")) {
        return ue4_17::IHeadMountedDisplayVT::get();
    }

    // 4.16
    if (version.dwFileVersionMS == 0x40010 || str_version.starts_with("4.16")) {
        return ue4_16::IHeadMountedDisplayVT::get();
    }

    // 4.15
    if (version.dwFileVersionMS == 0x4000F || str_version.starts_with("4.15")) {
        return ue4_15::IHeadMountedDisplayVT::get();
    }

    // 4.14
    if (version.dwFileVersionMS == 0x4000E || str_version.starts_with("4.14")) {
        return ue4_14::IHeadMountedDisplayVT::get();
    }

    // 4.13
    if (version.dwFileVersionMS == 0x4000D || str_version.starts_with("4.13")) {
        return ue4_13::IHeadMountedDisplayVT::get();
    }

    // 4.12
    if (version.dwFileVersionMS == 0x4000C || str_version.starts_with("4.12")) {
        return ue4_12::IHeadMountedDisplayVT::get();
    }

    // 4.11
    if (version.dwFileVersionMS == 0x4000B || str_version.starts_with("4.11")) {
        return ue4_11::IHeadMountedDisplayVT::get();
    }

    // 4.10
    if (version.dwFileVersionMS == 0x4000A || str_version.starts_with("4.10")) {
        return ue4_10::IHeadMountedDisplayVT::get();
    }

    return detail::IHeadMountedDisplayVT::get();
}

IXRTrackingSystemHook* g_hook = nullptr;

namespace detail {
constexpr uintptr_t DAYS_GONE_GAME_ENGINE_GAME_INSTANCE_OFFSET = 0xBE8;
constexpr uintptr_t DAYS_GONE_GAME_INSTANCE_LOCAL_PLAYERS_OFFSET = 0x38;
constexpr uintptr_t DAYS_GONE_LOCAL_PLAYER_CONTROLLER_OFFSET = 0x30;
constexpr uintptr_t DAYS_GONE_PLAYER_CONTROLLER_PAWN_OFFSET = 0x3B8;
constexpr uintptr_t DAYS_GONE_PLAYER_CONTROLLER_HUD_OFFSET = 0x3D0;
constexpr uintptr_t DAYS_GONE_BEND_HUD_SLATE_HUD_OFFSET = 0x480;
constexpr uintptr_t DAYS_GONE_SLATE_HUD_WIDGET_OFFSET = 0xB0;
constexpr uintptr_t DAYS_GONE_HUD_WIDGET_RETICLES_OFFSET = 0x528;
constexpr uintptr_t DAYS_GONE_RETICLES_IS_AIMING_OFFSET = 0x2C8;
constexpr uintptr_t DAYS_GONE_RETICLES_HIDDEN_OFFSET = 0x359;
constexpr uintptr_t DAYS_GONE_RETICLES_CURRENT_RETICLE_OFFSET = 0x380;
struct DaysGoneReticleDescriptor {
    uintptr_t child_offset{};
    uintptr_t visual_offset{};
    const char* name{};
};

constexpr std::array<DaysGoneReticleDescriptor, 6> DAYS_GONE_RETICLE_DESCRIPTORS{{
    {0x270, 0x2B0, "Assault"},
    {0x278, 0x278, "Crossbow"},
    {0x280, 0x2A0, "Marksman"},
    {0x288, 0x260, "Pistol"},
    // Scope is a full-screen overlay without a weapon Wrapper. Moving either
    // anonymous image would move the vignette, so preserve its native layout.
    {0x290, 0x000, "Scope"},
    {0x298, 0x288, "Shotgun"}
}};
constexpr uintptr_t DAYS_GONE_WIDGET_VISIBILITY_OFFSET = 0x91;
constexpr uintptr_t DAYS_GONE_WIDGET_RENDER_TRANSLATION_OFFSET = 0xB0;
constexpr float DAYS_GONE_HUD_DESIGN_WIDTH = 1920.0f;
constexpr float DAYS_GONE_HUD_DESIGN_HEIGHT = 1080.0f;
constexpr uintptr_t DAYS_GONE_PAWN_AIM_STANCE_OFFSET = 0x1E90;
constexpr uintptr_t DAYS_GONE_PAWN_WEAPON_MANAGER_OFFSET = 0x1B98;
constexpr uintptr_t DAYS_GONE_PAWN_EQUIPPED_WEAPON_OFFSET = 0x3568;
constexpr uintptr_t DAYS_GONE_WEAPON_MANAGER_EQUIPPED_ITEMS_OFFSET = 0xE8;
constexpr uintptr_t DAYS_GONE_WEAPON_MANAGER_DESIRED_WEAPON_OFFSET = 0x110;
constexpr uintptr_t DAYS_GONE_WEAPON_INSTIGATOR_OFFSET = 0x150;
constexpr uintptr_t DAYS_GONE_WEAPON_OWNER_IS_PLAYER_OFFSET = 0x3B0;
constexpr uintptr_t DAYS_GONE_WEAPON_AIM_AT_DIR_OFFSET = 0x1884;
constexpr uintptr_t DAYS_GONE_WEAPON_TARGET_AIM_AT_DIR_OFFSET = 0x1890;
constexpr uintptr_t DAYS_GONE_WEAPON_AIM_AT_POINT_OFFSET = 0x1878;
constexpr uintptr_t DAYS_GONE_WEAPON_OVERRIDE_AIM_POINT_OFFSET = 0x18A0;
constexpr uintptr_t DAYS_GONE_WEAPON_OVERRIDE_AIM_FLAG_OFFSET = 0x18B0;
constexpr size_t DAYS_GONE_WEAPON_AIM_TRACE_VTABLE_INDEX = 0x750 / sizeof(void*);
constexpr size_t DAYS_GONE_FUOBJECT_ITEM_SIZE = 0x10;
constexpr uintptr_t DAYS_GONE_FUOBJECT_ITEM_SERIAL_OFFSET = 0xC;
constexpr uint64_t DAYS_GONE_AIM_SAMPLE_MAX_AGE_MS = 250;
constexpr uint32_t DAYS_GONE_UNREACHABLE_OBJECT_FLAGS = 0x30000000;

struct DaysGoneArray {
    uintptr_t data{};
    int32_t count{};
    int32_t max{};
};

struct DaysGoneVector {
    float x{};
    float y{};
    float z{};
};

struct DaysGoneVec2 {
    float x{};
    float y{};
};

struct DaysGoneWeakObjectPtr {
    int32_t object_index{-1};
    int32_t object_serial{};
};

static_assert(sizeof(DaysGoneArray) == 0x10);
static_assert(sizeof(DaysGoneVector) == 0xC);
static_assert(sizeof(DaysGoneVec2) == 0x8);
static_assert(sizeof(DaysGoneWeakObjectPtr) == 0x8);

struct FunctionInfo {
    size_t functions_within{0};
    bool calls_xr_camera{false};
    bool calls_update_player_camera{false};
    bool calls_apply_hmd_rotation{false};
    bool process_view_rotation_analysis_failed{false};
};

std::mutex return_address_to_functions_mutex{};
std::unordered_map<uintptr_t, uintptr_t> return_address_to_functions{};
std::unordered_map<uintptr_t, FunctionInfo> functions{};
std::atomic<uint32_t> total_times_funcs_called{0};

bool is_writable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto base = (uintptr_t)mbi.BaseAddress;
    if (address + size > base + mbi.RegionSize) {
        return false;
    }

    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY ||
           protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY;
}

static bool is_readable_process_range(uintptr_t address, size_t size) {
    if (address == 0 || size == 0 || address + size < address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    return address + size <= base + mbi.RegionSize &&
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
}

static bool is_executable_process_address(uintptr_t address) {
    if (!is_readable_process_range(address, 1)) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    const auto protect = mbi.Protect & 0xff;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ ||
           protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
}

template <typename T>
static bool read_process_value(uintptr_t address, T& value) {
    if (!is_readable_process_range(address, sizeof(T))) {
        return false;
    }

    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

static uint64_t steady_clock_milliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool is_live_daysgone_object(void* object) try {
    if (!is_daysgone_executable() || object == nullptr ||
        !is_readable_process_range(reinterpret_cast<uintptr_t>(object), 0x28)) {
        return false;
    }

    int32_t index{};
    uintptr_t object_class{};
    if (!read_process_value(reinterpret_cast<uintptr_t>(object) + 0xC, index) ||
        !read_process_value(reinterpret_cast<uintptr_t>(object) + 0x10, object_class) ||
        object_class == 0 || !is_readable_process_range(object_class, 0x28)) {
        return false;
    }

    const auto objects = sdk::FUObjectArray::get();
    if (objects == nullptr || index < 0 || index >= objects->get_object_count()) {
        return false;
    }

    const auto item = objects->get_object(index);
    return item != nullptr && item->get_object() == object &&
        (item->get_flags() & DAYS_GONE_UNREACHABLE_OBJECT_FLAGS) == 0;
} catch (...) {
    return false;
}

static void* resolve_daysgone_weak_object(const DaysGoneWeakObjectPtr& weak) try {
    const auto objects = sdk::FUObjectArray::get();
    if (!is_daysgone_executable() || objects == nullptr || weak.object_index < 0 ||
        weak.object_index >= objects->get_object_count() || weak.object_serial <= 0 ||
        sdk::FUObjectArray::get_item_distance() != DAYS_GONE_FUOBJECT_ITEM_SIZE) {
        return nullptr;
    }

    const auto item = objects->get_object(weak.object_index);
    int32_t item_serial{};
    if (item == nullptr ||
        !read_process_value(
            reinterpret_cast<uintptr_t>(item) + DAYS_GONE_FUOBJECT_ITEM_SERIAL_OFFSET,
            item_serial) ||
        item_serial != weak.object_serial ||
        (item->get_flags() & DAYS_GONE_UNREACHABLE_OBJECT_FLAGS) != 0) {
        return nullptr;
    }

    auto* const object = item->get_object();
    return is_live_daysgone_object(object) ? object : nullptr;
} catch (...) {
    return nullptr;
}

static bool is_daysgone_player_weapon(void* weapon, uintptr_t pawn) {
    if (weapon == nullptr || pawn == 0 || !is_live_daysgone_object(weapon)) {
        return false;
    }

    uint8_t owner_is_player{};
    uintptr_t instigator{};
    return read_process_value(
               reinterpret_cast<uintptr_t>(weapon) + DAYS_GONE_WEAPON_OWNER_IS_PLAYER_OFFSET,
               owner_is_player) &&
        owner_is_player != 0 &&
        read_process_value(
            reinterpret_cast<uintptr_t>(weapon) + DAYS_GONE_WEAPON_INSTIGATOR_OFFSET,
            instigator) &&
        instigator == pawn;
}

static void* resolve_daysgone_player_weapon(uintptr_t pawn) {
    uintptr_t direct_weapon{};
    if (read_process_value(pawn + DAYS_GONE_PAWN_EQUIPPED_WEAPON_OFFSET, direct_weapon)) {
        auto* const weapon = reinterpret_cast<void*>(direct_weapon);
        if (is_daysgone_player_weapon(weapon, pawn)) {
            return weapon;
        }
    }

    uintptr_t manager{};
    if (!read_process_value(pawn + DAYS_GONE_PAWN_WEAPON_MANAGER_OFFSET, manager) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(manager))) {
        return nullptr;
    }

    DaysGoneWeakObjectPtr desired_weapon{};
    if (read_process_value(
            manager + DAYS_GONE_WEAPON_MANAGER_DESIRED_WEAPON_OFFSET,
            desired_weapon)) {
        auto* const weapon = resolve_daysgone_weak_object(desired_weapon);
        if (is_daysgone_player_weapon(weapon, pawn)) {
            return weapon;
        }
    }

    DaysGoneArray equipped_items{};
    if (!read_process_value(
            manager + DAYS_GONE_WEAPON_MANAGER_EQUIPPED_ITEMS_OFFSET,
            equipped_items) ||
        equipped_items.count < 1 || equipped_items.count > equipped_items.max ||
        equipped_items.max > 32 ||
        !is_readable_process_range(
            equipped_items.data,
            static_cast<size_t>(equipped_items.count) * sizeof(DaysGoneWeakObjectPtr))) {
        return nullptr;
    }

    for (int32_t i = 0; i < equipped_items.count; ++i) {
        DaysGoneWeakObjectPtr weak{};
        if (!read_process_value(
                equipped_items.data + static_cast<uintptr_t>(i) * sizeof(weak),
                weak)) {
            continue;
        }

        auto* const weapon = resolve_daysgone_weak_object(weak);
        if (is_daysgone_player_weapon(weapon, pawn)) {
            return weapon;
        }
    }

    return nullptr;
}

static bool validate_daysgone_weapon_aim_trace_helper(uintptr_t candidate) {
    if (!is_daysgone_executable() || !is_executable_process_address(candidate) ||
        utility::get_module_within(candidate).value_or(nullptr) != utility::get_executable()) {
        return false;
    }

    // Shared BendWeapon trace helper: preserve R9/R8, save the nonvolatile
    // registers, then consume endpoint.xyz and start.xyz. This exact ABI is
    // common to the base weapon and projectile weapon overrides.
    constexpr std::array<uint8_t, 47> expected_prologue{
        0x4C, 0x89, 0x4C, 0x24, 0x20,
        0x4C, 0x89, 0x44, 0x24, 0x18,
        0x53, 0x55, 0x56, 0x57,
        0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x83, 0xEC, 0x78,
        0x4C, 0x8B, 0xFA,
        0xF2, 0x41, 0x0F, 0x10, 0x11,
        0x45, 0x8B, 0x51, 0x08,
        0xF2, 0x41, 0x0F, 0x10, 0x08,
        0x41, 0x8B, 0x40, 0x08
    };

    return is_readable_process_range(candidate, expected_prologue.size()) &&
        std::memcmp(
            reinterpret_cast<const void*>(candidate),
            expected_prologue.data(),
            expected_prologue.size()) == 0;
}

static bool validate_daysgone_weapon_aim_trace_update(uintptr_t candidate, uintptr_t& trace_hook_point) {
    trace_hook_point = 0;

    if (!is_daysgone_executable() || !is_executable_process_address(candidate) ||
        utility::get_module_within(candidate).value_or(nullptr) != utility::get_executable()) {
        return false;
    }

    uint32_t matches = 0;
    bool saw_return = false;
    for (uintptr_t ip = candidate; ip < candidate + 0x1200;) {
        if (!is_readable_process_range(ip, 16)) {
            return false;
        }

        const auto decoded = utility::decode_one(reinterpret_cast<uint8_t*>(ip));
        if (!decoded || decoded->Length == 0) {
            return false;
        }

        const std::string_view mnemonic{decoded->Mnemonic};
        if (decoded->InstructionBytes[0] == 0xE8) {
            if (const auto target = utility::resolve_displacement(ip, &*decoded);
                target && validate_daysgone_weapon_aim_trace_helper(*target)) {
                trace_hook_point = *target;
                ++matches;
            }
        }

        if (mnemonic.starts_with("RET")) {
            saw_return = true;
            break;
        }

        ip += decoded->Length;
    }

    return saw_return && matches == 1 && trace_hook_point != 0;
}

static void* resolve_daysgone_active_reticle_visual(
    uintptr_t controller,
    uint8_t& current_reticle,
    void*& root_reticle,
    const char*& reticle_name) {
    current_reticle = 0xff;
    root_reticle = nullptr;
    reticle_name = nullptr;

    uintptr_t hud{};
    uintptr_t slate_hud{};
    uintptr_t hud_widget{};
    uintptr_t reticles{};
    if (!read_process_value(controller + DAYS_GONE_PLAYER_CONTROLLER_HUD_OFFSET, hud) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(hud)) ||
        !read_process_value(hud + DAYS_GONE_BEND_HUD_SLATE_HUD_OFFSET, slate_hud) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(slate_hud)) ||
        !read_process_value(slate_hud + DAYS_GONE_SLATE_HUD_WIDGET_OFFSET, hud_widget) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(hud_widget)) ||
        !read_process_value(hud_widget + DAYS_GONE_HUD_WIDGET_RETICLES_OFFSET, reticles) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(reticles))) {
        return nullptr;
    }

    uint8_t is_aiming{};
    uint8_t reticle_hidden{};
    if (!read_process_value(reticles + DAYS_GONE_RETICLES_IS_AIMING_OFFSET, is_aiming) ||
        !read_process_value(reticles + DAYS_GONE_RETICLES_HIDDEN_OFFSET, reticle_hidden) ||
        !read_process_value(reticles + DAYS_GONE_RETICLES_CURRENT_RETICLE_OFFSET, current_reticle) ||
        is_aiming == 0 || reticle_hidden != 0 || current_reticle >= 5) {
        return nullptr;
    }

    const DaysGoneReticleDescriptor* visible_descriptor{};
    uintptr_t visible_reticle{};
    uint32_t visible_count{};
    for (const auto& descriptor : DAYS_GONE_RETICLE_DESCRIPTORS) {
        uintptr_t child{};
        uint8_t visibility{};
        if (!read_process_value(reticles + descriptor.child_offset, child) ||
            !is_live_daysgone_object(reinterpret_cast<void*>(child)) ||
            !read_process_value(child + DAYS_GONE_WIDGET_VISIBILITY_OFFSET, visibility) ||
            visibility > 4) {
            continue;
        }

        // ESlateVisibility::Visible, HitTestInvisible, and
        // SelfHitTestInvisible all draw. Collapsed and Hidden do not.
        if (visibility == 0 || visibility == 3 || visibility == 4) {
            visible_descriptor = &descriptor;
            visible_reticle = child;
            ++visible_count;
        }
    }

    if (visible_count != 1 || visible_descriptor == nullptr ||
        visible_descriptor->visual_offset == 0) {
        return nullptr;
    }

    uintptr_t visual{};
    if (!read_process_value(
            visible_reticle + visible_descriptor->visual_offset,
            visual) ||
        !is_live_daysgone_object(reinterpret_cast<void*>(visual))) {
        return nullptr;
    }

    root_reticle = reinterpret_cast<void*>(visible_reticle);
    reticle_name = visible_descriptor->name;
    return reinterpret_cast<void*>(visual);
}

static bool project_daysgone_aim_to_reticle(const glm::vec3& desired, DaysGoneVec2& translation) {
    if (!std::isfinite(desired.x) || !std::isfinite(desired.y) ||
        !std::isfinite(desired.z) || desired.x <= 0.05f) {
        return false;
    }

    const auto project_eye = [&desired](const Matrix4x4f& projection, glm::vec2& ndc_delta) {
        for (uint32_t column = 0; column < 4; ++column) {
            for (uint32_t row = 0; row < 4; ++row) {
                if (!std::isfinite(projection[column][row])) {
                    return false;
                }
            }
        }

        // UEVR's projection is the Unreal row-vector matrix represented in
        // GLM storage. Reorder UE forward/right/up into projection x/y/z, then
        // subtract straight-ahead so asymmetric eye frusta do not bias the HUD.
        const auto clip = projection * glm::vec4{desired.y, desired.z, desired.x, 1.0f};
        const auto center = projection * glm::vec4{0.0f, 0.0f, 1.0f, 1.0f};
        if (!std::isfinite(clip.w) || !std::isfinite(center.w) ||
            std::abs(clip.w) < 0.0001f || std::abs(center.w) < 0.0001f) {
            return false;
        }

        const auto projected = glm::vec2{clip.x, clip.y} / clip.w;
        const auto projected_center = glm::vec2{center.x, center.y} / center.w;
        ndc_delta = projected - projected_center;
        return std::isfinite(ndc_delta.x) && std::isfinite(ndc_delta.y);
    };

    glm::vec2 left{};
    glm::vec2 right{};
    const auto& vr = VR::get();
    if (!project_eye(vr->get_projection_matrix(VRRuntime::Eye::LEFT), left) ||
        !project_eye(vr->get_projection_matrix(VRRuntime::Eye::RIGHT), right)) {
        return false;
    }

    auto ndc = (left + right) * 0.5f;
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) ||
        std::abs(ndc.x) > 8.0f || std::abs(ndc.y) > 8.0f) {
        return false;
    }

    // Keep a valid reticle on the visible HUD when the requested ray leaves
    // the current HMD frustum rather than allowing an unbounded widget offset.
    ndc.x = std::clamp(ndc.x, -0.98f, 0.98f);
    ndc.y = std::clamp(ndc.y, -0.98f, 0.98f);
    translation.x = ndc.x * (DAYS_GONE_HUD_DESIGN_WIDTH * 0.5f);
    translation.y = -ndc.y * (DAYS_GONE_HUD_DESIGN_HEIGHT * 0.5f);
    return std::isfinite(translation.x) && std::isfinite(translation.y);
}

static bool set_daysgone_reticle_translation(void* widget, const DaysGoneVec2& translation) try {
    if (!is_live_daysgone_object(widget) ||
        !std::isfinite(translation.x) || !std::isfinite(translation.y)) {
        return false;
    }

    struct Params {
        DaysGoneVec2 translation;
    } params{translation};

    reinterpret_cast<sdk::UObjectBase*>(widget)->call_function(L"SetRenderTranslation", &params);
    return true;
} catch (...) {
    return false;
}

template <typename T>
bool can_write(T* ptr) {
    return ptr != nullptr && is_writable_process_range((uintptr_t)ptr, sizeof(T));
}

bool finite_vec3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finite_quat(const glm::quat& q) {
    return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
}

bool finite_euler(const glm::vec3& v) {
    return finite_vec3(v);
}
}

IXRTrackingSystemHook::IXRTrackingSystemHook(FFakeStereoRenderingHook* stereo_hook, size_t offset_in_engine) 
    : m_stereo_hook{stereo_hook},
    m_offset_in_engine{offset_in_engine}
{
    SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook");

    g_hook = this;

    pre_initialize();
}

void IXRTrackingSystemHook::pre_initialize() {
    SPDLOG_INFO("IXRTrackingSystemHook::pre_initialize");

    for (auto i = 0; i < m_xrtracking_vtable.size(); ++i) {
        m_xrtracking_vtable[i] = (uintptr_t)+[](void*) {
            return nullptr;
        };
    }

    for (auto i = 0; i < m_camera_vtable.size(); ++i) {
        m_camera_vtable[i] = (uintptr_t)+[](void*) {
            return nullptr;
        };
    }

    for (auto i = 0; i < m_hmd_vtable.size(); ++i) {
        m_hmd_vtable[i] = (uintptr_t)+[](void*) {
            return nullptr;
        };
    }

    for (auto i = 0; i < m_stereo_rendering_vtable.size(); ++i) {
        m_stereo_rendering_vtable[i] = (uintptr_t)+[](void*) {
            return nullptr;
        };
    }

    for (auto i = 0; i < m_view_extension_vtable.size(); ++i) {
        m_view_extension_vtable[i] = (uintptr_t)+[](void*) {
            return nullptr;
        };
    }

    // GetSystemName
    m_xrtracking_vtable[0] = (uintptr_t)+[](void* this_ptr, sdk::FName* out) -> sdk::FName* {
        static sdk::FName fake_name{};
        return &fake_name;
    };

    const auto version = sdk::get_file_version_info();
    const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));
    
    try {
        const auto first_half = std::stoi(str_version.substr(0, str_version.find('.')));
        const auto second_half = std::stoi(str_version.substr(str_version.find('.') + 1, str_version.size() - 1));

        if (first_half == 4 && second_half == 26) {
            m_is_4_26 = true;
        }

        if (first_half == 4 && second_half <= 25) {
            SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook: version <= 4.25");
            m_is_leq_4_25 = true;
        }

        if (first_half == 4 && second_half <= 17) {
            SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook: version <= 4.17");
            m_is_leq_4_17 = true;
        }
    } catch(...) {
        SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: failed to convert second half of version string to number");
    }

    if (!m_is_leq_4_25 && version.dwFileVersionMS >= 0x40000 && version.dwFileVersionMS <= 0x40019) {
        SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook: version <= 4.25");
        m_is_leq_4_25 = true;
    }

    if (!m_is_leq_4_17 && version.dwFileVersionMS >= 0x40000 && version.dwFileVersionMS <= 0x40011) {
        SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook: version <= 4.17");
        m_is_leq_4_17 = true;
    }
}

void IXRTrackingSystemHook::on_draw_ui() {
}

bool IXRTrackingSystemHook::try_install_daysgone_weapon_aim_bridge(void* weapon) {
    if (!is_daysgone_executable() || weapon == nullptr || !detail::is_live_daysgone_object(weapon)) {
        return false;
    }

    if (m_daysgone_weapon_aim_trace_hook) {
        return validate_daysgone_weapon_object(weapon);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < m_daysgone_weapon_aim_next_retry) {
        return false;
    }
    m_daysgone_weapon_aim_next_retry = now + std::chrono::seconds(1);

    uintptr_t vtable{};
    if (!detail::read_process_value(reinterpret_cast<uintptr_t>(weapon), vtable) ||
        !detail::is_readable_process_range(
            vtable,
            (detail::DAYS_GONE_WEAPON_AIM_TRACE_VTABLE_INDEX + 1) * sizeof(void*))) {
        return false;
    }

    if (vtable == m_daysgone_rejected_weapon_vtable) {
        return false;
    }

    uintptr_t candidate{};
    uintptr_t trace_hook_point{};
    if (!detail::read_process_value(
            vtable + detail::DAYS_GONE_WEAPON_AIM_TRACE_VTABLE_INDEX * sizeof(void*),
            candidate) ||
        !detail::validate_daysgone_weapon_aim_trace_update(candidate, trace_hook_point)) {
        m_daysgone_rejected_weapon_vtable = vtable;
        SPDLOG_ERROR_ONCE(
            "[DaysGone][WeaponAim] Rejected the equipped item's vtable slot 0x750 trace topology; preserving native game aim");
        return false;
    }

    m_daysgone_weapon_aim_trace_hook = safetyhook::create_mid(
        reinterpret_cast<void*>(trace_hook_point),
        &IXRTrackingSystemHook::daysgone_weapon_aim_trace);
    if (!m_daysgone_weapon_aim_trace_hook) {
        SPDLOG_ERROR_EVERY_N_SEC(
            2,
            "[DaysGone][WeaponAim] Failed to hook validated BendWeapon aiming trace at 0x{:x}",
            trace_hook_point);
        return false;
    }

    m_daysgone_weapon_aim_trace_update = candidate;
    SPDLOG_INFO(
        "[DaysGone][WeaponAim] Hooked validated BendWeapon aiming trace at 0x{:x} "
        "(update=0x{:x}, vtable slot 0x750)",
        trace_hook_point,
        candidate);
    return true;
}

bool IXRTrackingSystemHook::validate_daysgone_weapon_object(void* weapon) const {
    if (!is_daysgone_executable() || weapon == nullptr || m_daysgone_weapon_aim_trace_update == 0 ||
        !detail::is_live_daysgone_object(weapon)) {
        return false;
    }

    uintptr_t vtable{};
    uintptr_t candidate{};
    return detail::read_process_value(reinterpret_cast<uintptr_t>(weapon), vtable) &&
        detail::is_readable_process_range(
            vtable,
            (detail::DAYS_GONE_WEAPON_AIM_TRACE_VTABLE_INDEX + 1) * sizeof(void*)) &&
        detail::read_process_value(
            vtable + detail::DAYS_GONE_WEAPON_AIM_TRACE_VTABLE_INDEX * sizeof(void*),
            candidate) &&
        candidate == m_daysgone_weapon_aim_trace_update;
}

void IXRTrackingSystemHook::invalidate_daysgone_weapon_aim_sample() {
    m_daysgone_active_pawn.store(nullptr, std::memory_order_release);

    m_daysgone_desired_aim_sequence.fetch_add(1, std::memory_order_acq_rel);
    m_daysgone_desired_aim_x.store(0.0f, std::memory_order_relaxed);
    m_daysgone_desired_aim_y.store(0.0f, std::memory_order_relaxed);
    m_daysgone_desired_aim_z.store(0.0f, std::memory_order_relaxed);
    m_daysgone_desired_aim_sample_time_ms.store(0, std::memory_order_relaxed);
    m_daysgone_desired_aim_sequence.fetch_add(1, std::memory_order_release);
}

void IXRTrackingSystemHook::restore_daysgone_reticle_alignment() {
    auto* const visual = m_daysgone_reticle_visual;
    if (visual != nullptr && m_daysgone_reticle_original_captured &&
        detail::is_live_daysgone_object(visual)) {
        detail::set_daysgone_reticle_translation(visual, {
            m_daysgone_reticle_original_translation.x,
            m_daysgone_reticle_original_translation.y
        });
    }

    m_daysgone_reticle_visual = nullptr;
    m_daysgone_reticle_original_translation = {};
    m_daysgone_reticle_original_captured = false;
}

void IXRTrackingSystemHook::update_daysgone_reticle_alignment(uintptr_t controller, const glm::vec3& desired) {
    uint8_t current_reticle{};
    void* root_reticle{};
    const char* reticle_name{};
    auto* const visual = detail::resolve_daysgone_active_reticle_visual(
        controller,
        current_reticle,
        root_reticle,
        reticle_name);
    detail::DaysGoneVec2 projected{};
    if (visual == nullptr || !detail::project_daysgone_aim_to_reticle(desired, projected)) {
        restore_daysgone_reticle_alignment();
        return;
    }

    if (visual != m_daysgone_reticle_visual) {
        restore_daysgone_reticle_alignment();

        detail::DaysGoneVec2 original{};
        if (!detail::read_process_value(
                reinterpret_cast<uintptr_t>(visual) + detail::DAYS_GONE_WIDGET_RENDER_TRANSLATION_OFFSET,
                original) ||
            !std::isfinite(original.x) || !std::isfinite(original.y) ||
            std::abs(original.x) > 4096.0f || std::abs(original.y) > 4096.0f) {
            return;
        }

        m_daysgone_reticle_visual = visual;
        m_daysgone_reticle_original_translation = {original.x, original.y};
        m_daysgone_reticle_original_captured = true;
        SPDLOG_INFO(
            "[DaysGone][ReticleAim] Following {} reticle type={} root=0x{:x} visual=0x{:x} baseline=({:.1f},{:.1f})",
            reticle_name,
            current_reticle,
            reinterpret_cast<uintptr_t>(root_reticle),
            reinterpret_cast<uintptr_t>(visual),
            original.x,
            original.y);
    }

    detail::DaysGoneVec2 target{
        m_daysgone_reticle_original_translation.x + projected.x,
        m_daysgone_reticle_original_translation.y + projected.y
    };
    detail::DaysGoneVec2 current{};
    if (!detail::read_process_value(
            reinterpret_cast<uintptr_t>(visual) + detail::DAYS_GONE_WIDGET_RENDER_TRANSLATION_OFFSET,
            current)) {
        restore_daysgone_reticle_alignment();
        return;
    }

    const auto differs = std::abs(current.x - target.x) > 0.25f ||
        std::abs(current.y - target.y) > 0.25f;
    if (differs && !detail::set_daysgone_reticle_translation(visual, target)) {
        restore_daysgone_reticle_alignment();
        return;
    }

    const auto now_ms = detail::steady_clock_milliseconds();
    if (now_ms >= m_daysgone_reticle_next_log_ms) {
        m_daysgone_reticle_next_log_ms = now_ms + 2000;
        SPDLOG_INFO(
            "[DaysGone][ReticleAim] {} type={} local=[{:.3f},{:.3f},{:.3f}] translation=({:.1f},{:.1f})",
            reticle_name,
            current_reticle,
            desired.x,
            desired.y,
            desired.z,
            target.x,
            target.y);
    }
}

bool IXRTrackingSystemHook::publish_daysgone_weapon_aim_sample(glm::vec3* published_desired) {
    auto& vr = VR::get();
    if (!is_daysgone_executable() || !vr->is_hmd_active() ||
        !vr->is_any_aim_method_active() || vr->is_controller_camera_conflict_guard_active()) {
        return false;
    }

    const auto aim_method = vr->get_aim_method();
    const auto rotation_offset = vr->get_rotation_offset();
    if (!detail::finite_quat(rotation_offset) || glm::dot(rotation_offset, rotation_offset) < 0.000001f) {
        return false;
    }

    glm::vec3 tracking_direction{};
    constexpr glm::vec3 openxr_forward{0.0f, 0.0f, -1.0f};

    if (aim_method == VR::AimMethod::HEAD) {
        const auto pose = glm::quat{vr->get_rotation(vr->get_hmd_index())};
        if (!detail::finite_quat(pose) || glm::dot(pose, pose) < 0.000001f) {
            return false;
        }

        tracking_direction = glm::normalize(rotation_offset) * (glm::normalize(pose) * openxr_forward);
    } else if (aim_method == VR::AimMethod::RIGHT_CONTROLLER ||
               aim_method == VR::AimMethod::LEFT_CONTROLLER) {
        if (!vr->is_using_controllers()) {
            return false;
        }

        const auto controller_index = aim_method == VR::AimMethod::RIGHT_CONTROLLER
            ? vr->get_right_controller_index()
            : vr->get_left_controller_index();
        if (controller_index < 0) {
            return false;
        }

        const auto pose = glm::quat{vr->get_aim_rotation(controller_index)};
        if (!detail::finite_quat(pose) || glm::dot(pose, pose) < 0.000001f) {
            return false;
        }

        tracking_direction = glm::normalize(rotation_offset) * (glm::normalize(pose) * openxr_forward);
    } else if (aim_method == VR::AimMethod::TWO_HANDED_RIGHT ||
               aim_method == VR::AimMethod::TWO_HANDED_LEFT) {
        if (!vr->is_using_controllers()) {
            return false;
        }

        const auto right_index = vr->get_right_controller_index();
        const auto left_index = vr->get_left_controller_index();
        if (right_index < 0 || left_index < 0) {
            return false;
        }

        const auto right = glm::vec3{vr->get_aim_position(right_index)};
        const auto left = glm::vec3{vr->get_aim_position(left_index)};
        const auto raw_direction = aim_method == VR::AimMethod::TWO_HANDED_RIGHT
            ? left - right
            : right - left;
        if (!detail::finite_vec3(raw_direction) || glm::dot(raw_direction, raw_direction) < 0.000001f) {
            return false;
        }

        tracking_direction = glm::normalize(rotation_offset) * glm::normalize(raw_direction);
    } else {
        return false;
    }

    if (!detail::finite_vec3(tracking_direction) ||
        glm::dot(tracking_direction, tracking_direction) < 0.000001f) {
        return false;
    }

    const auto desired = glm::normalize(
        utility::math::glm_to_ue4(glm::normalize(tracking_direction)));
    if (!detail::finite_vec3(desired)) {
        return false;
    }

    m_daysgone_desired_aim_sequence.fetch_add(1, std::memory_order_acq_rel);
    m_daysgone_desired_aim_x.store(desired.x, std::memory_order_relaxed);
    m_daysgone_desired_aim_y.store(desired.y, std::memory_order_relaxed);
    m_daysgone_desired_aim_z.store(desired.z, std::memory_order_relaxed);
    m_daysgone_desired_aim_sample_time_ms.store(
        detail::steady_clock_milliseconds(),
        std::memory_order_relaxed);
    m_daysgone_desired_aim_sequence.fetch_add(1, std::memory_order_release);
    if (published_desired != nullptr) {
        *published_desired = desired;
    }
    return true;
}

void IXRTrackingSystemHook::update_daysgone_weapon_aim_bridge(sdk::UGameEngine* engine) {
    const auto fail_open = [this]() {
        invalidate_daysgone_weapon_aim_sample();
        restore_daysgone_reticle_alignment();
    };

    if (!is_daysgone_native_aim_requested() || engine == nullptr || !VR::get()->is_hmd_active() ||
        VR::get()->is_controller_camera_conflict_guard_active()) {
        fail_open();
        return;
    }

    uintptr_t game_instance{};
    if (!detail::read_process_value(
            reinterpret_cast<uintptr_t>(engine) + detail::DAYS_GONE_GAME_ENGINE_GAME_INSTANCE_OFFSET,
            game_instance) ||
        !detail::is_live_daysgone_object(reinterpret_cast<void*>(game_instance))) {
        fail_open();
        return;
    }

    detail::DaysGoneArray local_players{};
    if (!detail::read_process_value(
            game_instance + detail::DAYS_GONE_GAME_INSTANCE_LOCAL_PLAYERS_OFFSET,
            local_players) ||
        local_players.count < 1 || local_players.count > local_players.max || local_players.max > 8 ||
        !detail::is_readable_process_range(local_players.data, sizeof(uintptr_t))) {
        fail_open();
        return;
    }

    uintptr_t local_player{};
    uintptr_t controller{};
    uintptr_t pawn{};
    if (!detail::read_process_value(local_players.data, local_player) ||
        !detail::is_live_daysgone_object(reinterpret_cast<void*>(local_player)) ||
        !detail::read_process_value(
            local_player + detail::DAYS_GONE_LOCAL_PLAYER_CONTROLLER_OFFSET,
            controller) ||
        !detail::is_live_daysgone_object(reinterpret_cast<void*>(controller)) ||
        !detail::read_process_value(
            controller + detail::DAYS_GONE_PLAYER_CONTROLLER_PAWN_OFFSET,
            pawn) ||
        !detail::is_live_daysgone_object(reinterpret_cast<void*>(pawn))) {
        fail_open();
        return;
    }

    uint8_t aim_stance{};
    if (!detail::read_process_value(pawn + detail::DAYS_GONE_PAWN_AIM_STANCE_OFFSET, aim_stance) ||
        (aim_stance != 1 && aim_stance != 2)) {
        fail_open();
        return;
    }

    if (!m_daysgone_weapon_aim_trace_hook) {
        auto* const weapon = detail::resolve_daysgone_player_weapon(pawn);
        if (weapon == nullptr ||
            !try_install_daysgone_weapon_aim_bridge(weapon) ||
            !validate_daysgone_weapon_object(weapon)) {
            fail_open();
            return;
        }

        SPDLOG_INFO(
            "[DaysGone][WeaponAim] Installed from validated player weapon at 0x{:x}",
            reinterpret_cast<uintptr_t>(weapon));
    }

    glm::vec3 desired{};
    if (!publish_daysgone_weapon_aim_sample(&desired)) {
        fail_open();
        return;
    }

    m_daysgone_active_pawn.store(reinterpret_cast<void*>(pawn), std::memory_order_release);
    update_daysgone_reticle_alignment(controller, desired);
}

void IXRTrackingSystemHook::daysgone_weapon_aim_trace(safetyhook::Context& ctx) {
    auto* const hook = g_hook;
    if (hook == nullptr || !hook->m_daysgone_weapon_aim_trace_hook) {
        return;
    }

    auto* const weapon = reinterpret_cast<void*>(ctx.rcx);
    auto* const active_pawn = hook->m_daysgone_active_pawn.load(std::memory_order_acquire);
    if (!is_daysgone_executable() || weapon == nullptr || active_pawn == nullptr ||
        !detail::is_live_daysgone_object(active_pawn)) {
        return;
    }

    uint8_t aim_stance{};
    uint8_t owner_is_player{};
    uintptr_t instigator{};
    if (!detail::read_process_value(
            reinterpret_cast<uintptr_t>(active_pawn) + detail::DAYS_GONE_PAWN_AIM_STANCE_OFFSET,
            aim_stance) ||
        (aim_stance != 1 && aim_stance != 2) ||
        !detail::read_process_value(
            reinterpret_cast<uintptr_t>(weapon) + detail::DAYS_GONE_WEAPON_OWNER_IS_PLAYER_OFFSET,
            owner_is_player) ||
        owner_is_player == 0 ||
        !detail::read_process_value(
            reinterpret_cast<uintptr_t>(weapon) + detail::DAYS_GONE_WEAPON_INSTIGATOR_OFFSET,
            instigator) ||
        instigator != reinterpret_cast<uintptr_t>(active_pawn)) {
        return;
    }

    detail::DaysGoneVector desired{};
    uint64_t sample_time{};
    bool stable_sample = false;
    for (uint32_t attempt = 0; attempt < 3; ++attempt) {
        const auto sequence_before =
            hook->m_daysgone_desired_aim_sequence.load(std::memory_order_acquire);
        if ((sequence_before & 1) != 0) {
            continue;
        }

        desired.x = hook->m_daysgone_desired_aim_x.load(std::memory_order_relaxed);
        desired.y = hook->m_daysgone_desired_aim_y.load(std::memory_order_relaxed);
        desired.z = hook->m_daysgone_desired_aim_z.load(std::memory_order_relaxed);
        sample_time = hook->m_daysgone_desired_aim_sample_time_ms.load(std::memory_order_relaxed);

        const auto sequence_after =
            hook->m_daysgone_desired_aim_sequence.load(std::memory_order_acquire);
        if (sequence_before == sequence_after && (sequence_after & 1) == 0) {
            stable_sample = true;
            break;
        }
    }

    const auto now_ms = detail::steady_clock_milliseconds();
    const auto desired_length_sq = desired.x * desired.x + desired.y * desired.y + desired.z * desired.z;
    if (!stable_sample || sample_time == 0 || now_ms < sample_time ||
        now_ms - sample_time > detail::DAYS_GONE_AIM_SAMPLE_MAX_AGE_MS ||
        !std::isfinite(desired.x) || !std::isfinite(desired.y) || !std::isfinite(desired.z) ||
        desired_length_sq < 0.98f || desired_length_sq > 1.02f) {
        return;
    }

    auto* const trace_start = reinterpret_cast<detail::DaysGoneVector*>(ctx.r8);
    auto* const trace_endpoint = reinterpret_cast<detail::DaysGoneVector*>(ctx.r9);
    if (!detail::is_readable_process_range(reinterpret_cast<uintptr_t>(trace_start), sizeof(*trace_start)) ||
        !detail::can_write(trace_endpoint)) {
        return;
    }

    const auto start = glm::vec3{trace_start->x, trace_start->y, trace_start->z};
    const auto native_endpoint = glm::vec3{trace_endpoint->x, trace_endpoint->y, trace_endpoint->z};
    const auto native_delta = native_endpoint - start;
    const auto native_distance_sq = glm::dot(native_delta, native_delta);
    if (!detail::finite_vec3(start) || !detail::finite_vec3(native_endpoint) ||
        !std::isfinite(native_distance_sq) || native_distance_sq < 1.0f || native_distance_sq > 1.0e12f) {
        return;
    }

    const auto native_distance = std::sqrt(native_distance_sq);
    const auto native_forward = native_delta / native_distance;
    constexpr glm::vec3 world_up{0.0f, 0.0f, 1.0f};
    const auto native_right_unnormalized = glm::cross(world_up, native_forward);
    const auto native_right_length_sq = glm::dot(native_right_unnormalized, native_right_unnormalized);
    if (!std::isfinite(native_right_length_sq) || native_right_length_sq < 0.0001f) {
        return;
    }

    const auto native_right = native_right_unnormalized / std::sqrt(native_right_length_sq);
    const auto native_up = glm::normalize(glm::cross(native_forward, native_right));
    const auto local_direction = glm::vec3{desired.x, desired.y, desired.z};
    const auto world_direction_unnormalized =
        native_forward * local_direction.x +
        native_right * local_direction.y +
        native_up * local_direction.z;
    const auto world_direction_length_sq = glm::dot(world_direction_unnormalized, world_direction_unnormalized);
    if (!detail::finite_vec3(native_forward) || !detail::finite_vec3(native_right) ||
        !detail::finite_vec3(native_up) || !detail::finite_vec3(world_direction_unnormalized) ||
        !std::isfinite(world_direction_length_sq) || world_direction_length_sq < 0.98f ||
        world_direction_length_sq > 1.02f) {
        return;
    }

    const auto world_direction = world_direction_unnormalized / std::sqrt(world_direction_length_sq);
    const auto rewritten_endpoint = start + world_direction * native_distance;
    if (!detail::finite_vec3(rewritten_endpoint)) {
        return;
    }

    trace_endpoint->x = rewritten_endpoint.x;
    trace_endpoint->y = rewritten_endpoint.y;
    trace_endpoint->z = rewritten_endpoint.z;

    auto next_log = hook->m_daysgone_aim_trace_next_log_ms.load(std::memory_order_relaxed);
    if (now_ms >= next_log && hook->m_daysgone_aim_trace_next_log_ms.compare_exchange_strong(
            next_log,
            now_ms + 1000,
            std::memory_order_relaxed)) {
        SPDLOG_INFO(
            "[DaysGone][WeaponAim] native=[{:.3f},{:.3f},{:.3f}] local=[{:.3f},{:.3f},{:.3f}] "
            "world=[{:.3f},{:.3f},{:.3f}] distance={:.1f}",
            native_forward.x,
            native_forward.y,
            native_forward.z,
            desired.x,
            desired.y,
            desired.z,
            world_direction.x,
            world_direction.y,
            world_direction.z,
            native_distance);
    }
}

void IXRTrackingSystemHook::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    auto& vr = VR::get();
    const auto direct_aim_compat_requested = is_direct_aim_compatibility_requested();
    const auto direct_aim_compat_active = is_direct_aim_compatibility_active();
    const auto deadzone_direct_aim = is_deadzone_ue56_executable();
    const auto legacy_ue4_direct_aim = is_ue4_14_through_4_17();
    const auto suppress_legacy_fake_hmd_for_aim = legacy_ue4_direct_aim && vr->is_any_aim_method_active();

    if (is_daysgone_executable()) {
        if (!is_daysgone_native_aim_requested()) {
            invalidate_daysgone_weapon_aim_sample();
            restore_daysgone_reticle_alignment();
        } else if (vr->is_controller_camera_conflict_guard_active()) {
            invalidate_daysgone_weapon_aim_sample();
            restore_daysgone_reticle_alignment();
            SPDLOG_WARN_ONCE(
                "[DaysGone][WeaponAim] Preserving native game aim because Controller-Camera Conflict Guard is active");
            return;
        } else if (vr->is_controller_aim_enabled() && !vr->is_using_controllers()) {
            invalidate_daysgone_weapon_aim_sample();
            restore_daysgone_reticle_alignment();
            SPDLOG_WARN_ONCE(
                "[DaysGone][WeaponAim] Waiting for motion-controller tracking without changing the configured aim method");
            return;
        } else {
            update_daysgone_weapon_aim_bridge(engine);
            // Days Gone non-game aim is owned by the weapon bridge. Do not also
            // mutate camera, ControlRotation, world scale, or the fake HMD path.
            return;
        }
    }

    if (direct_aim_compat_requested && vr->is_any_aim_method_active()) {
        const auto aim_method = vr->get_aim_method();

        if (legacy_ue4_direct_aim && vr->is_controller_camera_conflict_guard_active()) {
            SPDLOG_WARN_ONCE("[UE4.14-4.17][Aim] Direct aim is blocked by Controller-Camera Conflict Guard; legacy fake HMD remains disabled");
        } else if (aim_method == VR::AimMethod::HEAD) {
            if (legacy_ue4_direct_aim) {
                SPDLOG_WARN_ONCE("[UE4.14-4.17][Aim] Using direct HMD aim without installing the legacy fake IHeadMountedDisplay");
            } else if (deadzone_direct_aim) {
                SPDLOG_WARN_ONCE("[Deadzone][Aim] Allowing HMD aim on UE5.6 through ProcessViewRotation; unsafe direct UObject/FName fallback is disabled");
            } else {
                SPDLOG_WARN_ONCE("[AimCompat] Allowing experimental HMD aim through direct control rotation updates");
            }
        } else if (!vr->is_controller_aim_enabled() || !vr->is_using_controllers()) {
            if (legacy_ue4_direct_aim && vr->is_controller_aim_enabled()) {
                SPDLOG_WARNING_EVERY_N_SEC(2, "[UE4.14-4.17][Aim] Waiting for motion-controller tracking; legacy fake HMD remains disabled");
            } else if (deadzone_direct_aim) {
                SPDLOG_WARN_ONCE("[Deadzone][Aim] Falling back to game aim because controller aim is not actively available");
            } else {
                SPDLOG_WARN_ONCE("[AimCompat] Falling back to game aim because controller aim is not actively available");
            }
            if (!legacy_ue4_direct_aim) {
                vr->set_aim_method(VR::AimMethod::GAME);
                return;
            }
        } else {
            if (legacy_ue4_direct_aim) {
                SPDLOG_WARN_ONCE("[UE4.14-4.17][Aim] Using direct controller aim without installing the legacy fake IHeadMountedDisplay");
            } else if (deadzone_direct_aim) {
                SPDLOG_WARN_ONCE("[Deadzone][Aim] Allowing experimental controller aim on UE5.6; XR camera path remains disabled");
            } else {
                SPDLOG_WARN_ONCE("[AimCompat] Allowing experimental controller aim; XR camera path remains disabled");
            }
        }
    }

    if (!m_initialized &&
        ((vr->is_any_aim_method_active() && !direct_aim_compat_active && !suppress_legacy_fake_hmd_for_aim) ||
         vr->wants_blueprint_load())) {
        if (!m_initialized) {
            initialize();
        }
    }

    if (direct_aim_compat_active) {
        if (legacy_ue4_direct_aim) {
            SPDLOG_INFO_ONCE("[UE4.14-4.17][Aim] Driving guarded direct control-rotation updates");
        } else if (deadzone_direct_aim) {
            SPDLOG_INFO_ONCE("[Deadzone][Aim] Driving Deadzone direct aim through control rotation updates");
        } else {
            SPDLOG_INFO_ONCE("[AimCompat] Driving direct aim fallback through control rotation updates");
        }
        manual_update_control_rotation(engine);
    }

    auto& data = m_process_view_rotation_data;

    if (vr->is_any_aim_method_active()) {

        // This can happen if player logic stops running (e.g. player has died or entered a loading screen)
        // so we dont want the UI off in nowhere land
        if (data.was_called && std::chrono::high_resolution_clock::now() - data.last_update >= std::chrono::seconds(2)) {
            data.was_called = false;
            vr->recenter_view();
            vr->set_pre_flattened_rotation(glm::identity<glm::quat>());

            SPDLOG_INFO("IXRTrackingSystemHook: Recentering view because of timeout");
        }
    } else if (data.auto_enabled_decoupled_pitch) {
        data.auto_enabled_decoupled_pitch = false;
        vr->set_decoupled_pitch(false);
        vr->set_pre_flattened_rotation(glm::identity<glm::quat>());
        SPDLOG_INFO("[IXRTrackingSystemHook] Restored decoupled pitch after aim method was disabled");
    }
}

void IXRTrackingSystemHook::on_post_engine_tick(sdk::UGameEngine* engine, float delta) {
    if (is_daysgone_executable()) {
        return;
    }

    if (VR::get()->is_controller_camera_conflict_guard_active()) {
        return;
    }

    if (!is_direct_aim_compatibility_active()) {
        return;
    }

    manual_update_control_rotation(engine);
}

void IXRTrackingSystemHook::initialize() {
    m_initialized = true;

    SPDLOG_INFO("IXRTrackingSystemHook::initialize");

    if (sdk::UGameEngine::get() == nullptr) {
        SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: UGameEngine not found");
        return;
    }

    if (m_offset_in_engine == 0) {
        SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: m_offset_in_engine not set");
        return;
    }

    const auto& camera_vt = get_camera_vtable(m_overridden_version);
    const auto& trackvt = get_tracking_system_vtable(m_overridden_version);
    const auto& hmdvt = get_hmd_vtable(m_overridden_version);

    if (trackvt.implemented()) {
        if (trackvt.GetXRCamera_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetXRCamera_index().value()] = (uintptr_t)&get_xr_camera;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_xr_camera_index not implemented");
        }

        if (trackvt.IsHeadTrackingAllowed_index().has_value()) {
            m_xrtracking_vtable[trackvt.IsHeadTrackingAllowed_index().value()] = (uintptr_t)&is_head_tracking_allowed;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: is_head_tracking_allowed_index not implemented");
        }

        if (trackvt.IsHeadTrackingAllowedForWorld_index().has_value()) {
            m_xrtracking_vtable[trackvt.IsHeadTrackingAllowedForWorld_index().value()] = (uintptr_t)&is_head_tracking_allowed_for_world;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: is_head_tracking_allowed_for_world_index not implemented");
        }

        if (trackvt.GetMotionControllerData_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetMotionControllerData_index().value()] = (uintptr_t)&get_motion_controller_data;
        } else if (!trackvt.GetMotionControllerState_index().has_value()) {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_motion_controller_data_index not implemented");
        }

        if (trackvt.GetMotionControllerState_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetMotionControllerState_index().value()] = (uintptr_t)&get_motion_controller_state;
        }

        if (trackvt.GetHMDData_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetHMDData_index().value()] = (uintptr_t)&get_hmd_data;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_hmd_data_index not implemented");
        }

        if (trackvt.GetCurrentPose_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetCurrentPose_index().value()] = (uintptr_t)&get_current_pose;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_current_pose_index not implemented");
        }

        if (trackvt.GetXRSystemFlags_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetXRSystemFlags_index().value()] = (uintptr_t)&get_xr_system_flags;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_xr_system_flags_index not implemented");
        }

        // Doesn't cause a crash, but must be implemented to fix audio bugs
        if (trackvt.GetAudioListenerOffset_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetAudioListenerOffset_index().value()] = (uintptr_t)&get_audio_listener_offset;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_audio_listener_offset_index not implemented");
        }

        // Some games calls this for some reason so it needs to be implemented so we dont crash
        if (trackvt.GetBaseOrientation_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetBaseOrientation_index().value()] = (uintptr_t)&get_base_orientation;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_base_orientation_index not implemented");
        }

        if (trackvt.GetBasePosition_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetBasePosition_index().value()] = (uintptr_t)&get_base_position;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_base_position_index not implemented");
        }

        if (trackvt.GetBaseRotation_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetBaseRotation_index().value()] = (uintptr_t)&get_base_rotation;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_base_rotation_index not implemented");
        }

        if (trackvt.ResetOrientation_index().has_value()) {
            m_xrtracking_vtable[trackvt.ResetOrientation_index().value()] = (uintptr_t)&reset_orientation;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_orientation_index not implemented");
        }

        if (trackvt.ResetPosition_index().has_value()) {
            m_xrtracking_vtable[trackvt.ResetPosition_index().value()] = (uintptr_t)&reset_position;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_position_index not implemented");
        }

        if (trackvt.ResetOrientationAndPosition_index().has_value()) {
            m_xrtracking_vtable[trackvt.ResetOrientationAndPosition_index().value()] = (uintptr_t)&reset_orientation_and_position;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_orientation_and_position_index not implemented");
        }

        if (m_is_4_26) {
            if (trackvt.GetStereoRenderingDevice_index().has_value()) {
                m_xrtracking_vtable[trackvt.GetStereoRenderingDevice_index().value()] = (uintptr_t)&get_stereo_rendering_device;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_stereo_rendering_device_index not implemented");
            }
        } else {
            if (trackvt.GetStereoRenderingDevice_index().has_value()) {
                m_xrtracking_vtable[trackvt.GetStereoRenderingDevice_index().value()] = (uintptr_t)+[](void* a1, void* a2, void* a3) -> void* {
                    SPDLOG_INFO_ONCE("GetStereoRenderingDevice called");

                    return nullptr;
                };
            }
        }

        if (hmdvt.implemented() && trackvt.GetHMDDevice_index().has_value()) {
            m_xrtracking_vtable[trackvt.GetHMDDevice_index().value()] = (uintptr_t)+[]() -> void* {
                SPDLOG_INFO_ONCE("GetHMDDevice called");

                return &g_hook->m_hmd_device;
            };

            if (hmdvt.IsHMDConnected_index().has_value()) {
                m_hmd_vtable[hmdvt.IsHMDConnected_index().value()] = (uintptr_t)&is_hmd_connected;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: is_hmd_connected_index not implemented");
            }

            if (hmdvt.GetIdealRenderTargetSize_index().has_value()) {
                m_hmd_vtable[hmdvt.GetIdealRenderTargetSize_index().value()] = (uintptr_t)&get_ideal_debug_canvas_render_target_size;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_ideal_render_target_size_index not implemented");
            }

            if (hmdvt.GetIdealDebugCanvasRenderTargetSize_index().has_value()) {
                // This one is a bit tricky. In very rare cases this index can be off by one. We need to make the hook
                // verify that the return address is within UGameViewportClient::Draw. We will not just hook this function, but one index ahead as well.
                const auto debug_size_index = hmdvt.GetIdealDebugCanvasRenderTargetSize_index().value();
                m_hmd_vtable[debug_size_index] = (uintptr_t)&get_ideal_debug_canvas_render_target_size;

                // UE 5.7's HMD vtable was confirmed from PDB and the next slot is
                // GetDistortionScalingFactor, not an off-by-one debug-size call.
                if (hmdvt.BeginRendering_RenderThread_index().has_value()) {
                    m_hmd_vtable[debug_size_index + 1] = (uintptr_t)&get_ideal_debug_canvas_render_target_size;
                }
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_ideal_debug_canvas_render_target_size_index not implemented");
            }

            if (hmdvt.ResetOrientation_index().has_value()) {
                m_hmd_vtable[hmdvt.ResetOrientation_index().value()] = (uintptr_t)&reset_orientation;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_orientation_index not implemented");
            }

            if (hmdvt.ResetPosition_index().has_value()) {
                m_hmd_vtable[hmdvt.ResetPosition_index().value()] = (uintptr_t)&reset_position;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_position_index not implemented");
            }

            if (hmdvt.ResetOrientationAndPosition_index().has_value()) {
                m_hmd_vtable[hmdvt.ResetOrientationAndPosition_index().value()] = (uintptr_t)&reset_orientation_and_position;
            } else {
                SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: reset_orientation_and_position_index not implemented");
            }

            m_hmd_device.vtable = m_hmd_vtable.data();
            m_hmd_device.stereo_rendering_vtable = m_stereo_rendering_vtable.data();
        }
    } else if (hmdvt.implemented()) {
        SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook: IXRTrackingSystemVT not implemented, using IHeadMountedDisplayVT");

        if (hmdvt.IsHeadTrackingAllowed_index().has_value()) {
            m_hmd_vtable[hmdvt.IsHeadTrackingAllowed_index().value()] = (uintptr_t)&is_head_tracking_allowed;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: is_head_tracking_allowed_index not implemented");
        }

        if (hmdvt.ApplyHmdRotation_index().has_value()) {
            m_hmd_vtable[hmdvt.ApplyHmdRotation_index().value()] = (uintptr_t)&apply_hmd_rotation;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: apply_hmd_rotation_index not implemented");
        }

        if (hmdvt.IsHMDConnected_index().has_value()) {
            m_hmd_vtable[hmdvt.IsHMDConnected_index().value()] = (uintptr_t)&is_hmd_connected;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: is_hmd_connected_index not implemented");
        }

        if (hmdvt.GetAudioListenerOffset_index().has_value()) {
            m_hmd_vtable[hmdvt.GetAudioListenerOffset_index().value()] = (uintptr_t)&get_audio_listener_offset;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_audio_listener_offset_index not implemented");
        }

        if (hmdvt.UpdatePlayerCamera_index().has_value()) {
            m_hmd_vtable[hmdvt.UpdatePlayerCamera_index().value()] = (uintptr_t)&update_player_camera;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: update_player_camera_index not implemented");
        }

        if (hmdvt.GetViewExtension_index().has_value()) {
            m_hmd_vtable[hmdvt.GetViewExtension_index().value()] = (uintptr_t)&get_view_extension;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: get_view_extension_index not implemented");
        }
    } else {
        SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: IXRTrackingSystemVT and IXRHeadMountedDisplayVT not implemented");
    }

    if (camera_vt.implemented()) {
        if (camera_vt.ApplyHMDRotation_index().has_value()) {
            m_camera_vtable[camera_vt.ApplyHMDRotation_index().value()] = (uintptr_t)&apply_hmd_rotation;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: apply_hmd_rotation_index not implemented");
        }

        if (camera_vt.UpdatePlayerCamera_index().has_value()) {
            m_camera_vtable[camera_vt.UpdatePlayerCamera_index().value()] = (uintptr_t)&update_player_camera;
        } else {
            SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: update_player_camera_index not implemented");
        }
    } else {
        SPDLOG_ERROR("IXRTrackingSystemHook::IXRTrackingSystemHook: IXRCameraVT not implemented");
    }

    m_view_extension.vtable = m_view_extension_vtable.data();
    m_view_extension_shared.obj = &m_view_extension;
    m_view_extension_shared.ref_controller = nullptr;

    if (camera_vt.implemented()) {
        m_xr_camera.vtable = m_camera_vtable.data();
        m_xr_camera_shared.obj = &m_xr_camera;
    }

    if (trackvt.implemented()) { // >= 4.18
        auto tracking_system = (SharedPtr*)((uintptr_t)sdk::UGameEngine::get() + m_offset_in_engine);

        m_xr_tracking_system.vtable = m_xrtracking_vtable.data();
        tracking_system->obj = &m_xr_tracking_system;
        tracking_system->ref_controller = nullptr;
        //tracking_system->ref_controller = m_ref_controller.get();
    } else if (hmdvt.implemented()) { // <= 4.17
        auto hmd_device = (SharedPtr*)((uintptr_t)sdk::UGameEngine::get() + m_offset_in_engine);

        m_hmd_device.vtable = m_hmd_vtable.data();
        m_hmd_device.stereo_rendering_vtable = m_stereo_rendering_vtable.data();
        hmd_device->obj = &m_hmd_device;
        hmd_device->ref_controller = nullptr;
    }

    SPDLOG_INFO("IXRTrackingSystemHook::IXRTrackingSystemHook done");
}

IXRTrackingSystemHook::SharedPtr* IXRTrackingSystemHook::get_stereo_rendering_device(sdk::IXRTrackingSystem* This, IXRTrackingSystemHook::SharedPtr* out, void* a3) {
    // So we are not actually using this for anything right now, we are just using it to switch our vtables to 4.27 if needed.
    // Not actually aware of any instance where we would legitimately need to return a valid stereo device.
    if (!g_hook->m_is_4_26) {
        return nullptr;
    }

    SPDLOG_INFO_ONCE("IXRTrackingSystemHook::get_stereo_rendering_device");

    static std::mutex mtx{};
    static std::unordered_set<uintptr_t> invalid_return_addresses{};

    const auto return_address = (uintptr_t)_ReturnAddress(); 
    
    std::scoped_lock _{mtx};

    if (invalid_return_addresses.contains(return_address)) {
        return nullptr;
    }

    try {
        invalid_return_addresses.insert(return_address);

        // Setup the emulator. Set RAX to magic value. If RAX changes at any point, abort.
        const auto module_within = utility::get_module_within(return_address);

        if (!module_within) {
            SPDLOG_ERROR("IXRTrackingSystemHook::get_stereo_rendering_device: failed to get module within");

            return nullptr;
        }

        static auto check_ix = [](const INSTRUX& ix) {
            for (auto i = 0; i < ix.OperandsCount; ++i) {
                const auto& op = ix.Operands[i];

                if (op.Type != ND_OPERAND_TYPE::ND_OP_MEM) {
                    continue;
                }

                if (op.Info.Memory.HasBase && op.Info.Memory.Base == NDR_RAX) {
                    SPDLOG_INFO("Found dereference of RAX");
                    return true;
                }
            }

            return false;
        };

        const auto retdecode = utility::decode_one((uint8_t*)return_address);

        if (!retdecode) {
            SPDLOG_ERROR("IXRTrackingSystemHook::get_stereo_rendering_device: failed to decode instruction at 0x{:x}", return_address);
            return nullptr;
        }

        bool should_switch_to_4_27 = check_ix(*retdecode);

        if (!should_switch_to_4_27) {
            utility::ShemuContext base_context{*module_within};

            base_context.ctx->Registers.RegRip = return_address;
            base_context.ctx->Registers.RegRax = 0xdeadbeef;
            base_context.ctx->MemThreshold = 10;

            utility::emulate(*module_within, return_address, 10, base_context, [&should_switch_to_4_27](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                if (should_switch_to_4_27) {
                    return utility::ExhaustionResult::BREAK;
                }

                if (check_ix(ctx.ctx->ctx->Instruction) || check_ix(ctx.next.ix)) {
                    should_switch_to_4_27 = true;
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.next.ix.BranchInfo.IsBranch) {
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.ctx->ctx->Registers.RegRax != 0xdeadbeef) {
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.next.writes_to_memory) {
                    return utility::ExhaustionResult::STEP_OVER;
                }

                return utility::ExhaustionResult::CONTINUE;
            });
        }

        if (should_switch_to_4_27) {
            SPDLOG_INFO("IXRTrackingSystemHook::get_stereo_rendering_device: detected necessary switch to 4.27");

            // pre_initialize clears out all of our already set vtables to their default values
            g_hook->pre_initialize();
            g_hook->m_is_4_26 = false;
            g_hook->m_overridden_version = "4.27";
            // now set up the vtables again
            g_hook->initialize();

            // This is the real function that was supposed to be called.
            if (out != nullptr) {
                *out = g_hook->m_xr_camera_shared;
            }
            
            return out;
        }
    } catch(...) {
        SPDLOG_ERROR("IXRTrackingSystemHook::get_stereo_rendering_device: exception");
        invalid_return_addresses.insert(return_address);
    }

    return nullptr;
}

void IXRTrackingSystemHook::manual_update_control_rotation(sdk::UGameEngine* engine_override) {
    if (is_daysgone_executable()) {
        return;
    }

    if (VR::get()->is_controller_camera_conflict_guard_active()) {
        return;
    }

    if (is_deadzone_ue56_executable()) {
        SPDLOG_WARN_ONCE("[Deadzone][Aim] Direct PlayerController aim fallback is disabled because UObject/FName lookup is unsafe; using ProcessViewRotation instead");
        return;
    }

    const auto legacy_ue4_direct_aim = is_ue4_14_through_4_17();
    const auto guarded_legacy_aim = legacy_ue4_direct_aim;
    constexpr auto guarded_aim_name = "UE4.14-4.17";

    if (guarded_legacy_aim) {
        const auto& vr = VR::get();

        if ((vr->is_controller_aim_enabled() && !vr->is_using_controllers()) ||
            !is_legacy_aim_reflection_ready()) {
            SPDLOG_WARNING_EVERY_N_SEC(
                2,
                "[{}][Aim] Direct aim is fail-closed while controller tracking or UObject reflection is unavailable",
                guarded_aim_name);
            return;
        }
    }

    sdk::APlayerController* controller = nullptr;

    auto engine = (sdk::UEngine*)engine_override;
    if (engine == nullptr) {
        engine = sdk::UEngine::get();
    }

    if (engine == nullptr) {
        return;
    }

    const auto world = engine->get_world();

    if (world == nullptr) {
        return;
    }

    try {
        controller = resolve_player_controller_for_aim(engine, world);
    } catch (...) {
        if (guarded_legacy_aim) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] Failed to resolve a safe PlayerController", guarded_aim_name);
            return;
        }

        throw;
    }

    if (controller == nullptr) {
        SPDLOG_WARN_ONCE("[AimCompat] Skipping direct aim until a PlayerController is available");
        return;
    }

    if (guarded_legacy_aim && !is_live_legacy_aim_object(controller)) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] Refusing a stale or invalid PlayerController", guarded_aim_name);
        return;
    }

    sdk::APawn* pawn = nullptr;

    try {
        pawn = resolve_acknowledged_pawn_for_aim(controller);
    } catch (...) {
        if (guarded_legacy_aim) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] Failed to resolve a safe acknowledged pawn", guarded_aim_name);
            return;
        }

        throw;
    }

    if (pawn == nullptr || (guarded_legacy_aim && !is_live_legacy_aim_object(pawn))) {
        return;
    }

    glm::vec3 control_rotation{};

    if (is_payday3_aim_guard_enabled()) {
        if (!read_payday3_control_rotation_property(controller, control_rotation)) {
            return;
        }
    } else {
        try {
            control_rotation = controller->get_control_rotation();
        } catch (...) {
            if (guarded_legacy_aim) {
                SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] GetControlRotation failed; skipping this update", guarded_aim_name);
                return;
            }

            throw;
        }
    }

    if (guarded_legacy_aim && !detail::finite_euler(control_rotation)) {
        SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] Ignoring non-finite ControlRotation", guarded_aim_name);
        return;
    }
    
    Rotator<double> ue5_rotation {
        (double)control_rotation.x,
        (double)control_rotation.y,
        (double)control_rotation.z
    };

    const auto is_ue5 = g_hook->m_stereo_hook->has_double_precision();

    Rotator<float>* chosen_rotation = is_ue5 ? (Rotator<float>*)&ue5_rotation : (Rotator<float>*)&control_rotation;

    update_view_rotation(controller, chosen_rotation);
    
    // Conv back to vec3<float>
    glm::vec3 final_rotation{};

    if (is_ue5) {
        final_rotation = glm::vec3{
            (float)ue5_rotation.pitch,
            (float)ue5_rotation.yaw,
            (float)ue5_rotation.roll
        };
    } else {
        final_rotation = glm::vec3{
            control_rotation.x,
            control_rotation.y,
            control_rotation.z
        };
    }

    if (is_payday3_aim_guard_enabled()) {
        write_payday3_control_rotation_property(controller, final_rotation);
        return;
    }

    if (guarded_legacy_aim) {
        try {
            // UE4.14-4.17 use UESDK's reflected direct property path. Days Gone
            // remains on its validated SetControlRotation ProcessEvent path so
            // the native root-component rotation side effect is preserved.
            controller->set_control_rotation(final_rotation);
        } catch (...) {
            SPDLOG_WARNING_EVERY_N_SEC(2, "[{}][Aim] SetControlRotation failed; skipping this update", guarded_aim_name);
        }
    } else {
        controller->set_control_rotation(final_rotation);
    }
}

bool IXRTrackingSystemHook::analyze_head_tracking_allowed(uintptr_t return_address) {
    ++detail::total_times_funcs_called;

    std::scoped_lock _{detail::return_address_to_functions_mutex};

    auto it = detail::return_address_to_functions.find(return_address);

    if (it == detail::return_address_to_functions.end()) {
        const auto vfunc = utility::find_virtual_function_start(return_address);

        if (vfunc) {
            detail::return_address_to_functions[return_address] = *vfunc;
        } else {
            detail::return_address_to_functions[return_address] = 0;
        }

        it = detail::return_address_to_functions.find(return_address);
    }

    if (it->second == 0) {
        return false;
    }

    auto& func = detail::functions[it->second];

    if (!g_hook->m_process_view_rotation_hook && detail::total_times_funcs_called >= 100 && 
        !func.calls_xr_camera && !func.calls_update_player_camera && !func.calls_apply_hmd_rotation && !func.process_view_rotation_analysis_failed)
    {
        g_hook->m_attempted_hook_view_rotation = true;

        SPDLOG_INFO("Possibly found ProcessViewRotation: 0x{:x}", it->second);

        const auto module = utility::get_module_within(it->second);

        if (!module) {
            SPDLOG_ERROR("Failed to get module for ProcessViewRotation");
            return false;
        }

        const auto func_ptr = utility::scan_ptr(*module, it->second);

        if (!func_ptr) {
            SPDLOG_ERROR("Failed to find ProcessViewRotation");
            return false;
        }

        // We do not definitively hook ProcessViewRotation here, we point it towards a function analyzer
        // that checks whether the arguments match up with what we expect. If they do, we hook it.
        m_addr_of_process_view_rotation_ptr = *func_ptr;
        //g_hook->m_process_view_rotation_hook = std::make_unique<PointerHook>((void**)*func_ptr, (void*)&process_view_rotation_analyzer);
        g_hook->m_process_view_rotation_hook = safetyhook::create_inline((void*)it->second, (void*)&process_view_rotation_analyzer);

        if (!g_hook->m_process_view_rotation_hook) {
            SPDLOG_ERROR("Failed to hook ProcessViewRotation");
            func.process_view_rotation_analysis_failed = true;
            g_hook->m_attempted_hook_view_rotation = false;
            return true;
        }

        SPDLOG_INFO("Hooked ProcessViewRotation");
    }

    return true;
}

void* IXRTrackingSystemHook::get_orientation_and_position_native(void* rcx, void* rdx, void* r8, void* r9) {
    SPDLOG_INFO_ONCE("GetOrientationAndPosition native function {:x}", (uintptr_t)_ReturnAddress());

    const auto og = g_hook->m_native_get_oap_hook->get_original<decltype(&get_orientation_and_position_native)>();

    g_hook->m_within_get_oap_native = true;

    const auto result = og(rcx, rdx, r8, r9);

    g_hook->m_within_get_oap_native = false;

    return result;
}

void* IXRTrackingSystemHook::is_head_mounted_display_enabled_native(void* rcx, void* rdx, void* r8, void* r9) {
    SPDLOG_INFO_ONCE("IsHeadMountedDisplayEnabled native function {:x}", (uintptr_t)_ReturnAddress());

    const auto og = g_hook->m_native_is_hmd_enabled_hook->get_original<decltype(&is_head_mounted_display_enabled_native)>();

    g_hook->m_within_is_hmd_enabled_native = true;

    const auto result = og(rcx, rdx, r8, r9);

    g_hook->m_within_is_hmd_enabled_native = false;

    return result;
}

bool IXRTrackingSystemHook::is_head_tracking_allowed(sdk::IXRTrackingSystem*) {
    SPDLOG_INFO_ONCE("is_head_tracking_allowed {:x}", (uintptr_t)_ReturnAddress());

    auto& vr = VR::get();

    if (is_daysgone_executable() && vr->is_any_aim_method_active()) {
        return false;
    }

    if (is_direct_aim_compatibility_active()) {
        return false;
    }

    if (!vr->is_hmd_active()) {
        return false;
    }

    if (g_hook->m_is_leq_4_17) {
        return true;
    }

    static bool attempted_check = false;

    if (vr->wants_blueprint_load() && !attempted_check) try {
        attempted_check = true;
        const auto uobjectarray = sdk::FUObjectArray::get();

        if (uobjectarray == nullptr) {
            SPDLOG_ERROR("Failed to find FUObjectArray");
            return false;
        }

        const auto hmd_lib = sdk::UHeadMountedDisplayFunctionLibrary::static_class();

        if (hmd_lib == nullptr) {
            SPDLOG_ERROR("Failed to find UHeadMountedDisplayFunctionLibrary");
            return false;
        }

        if (sdk::UFunction::get_native_function_offset() == 0) {
            SPDLOG_ERROR("UFunction::get_native_function_offset is 0");
            return false;
        }

        // GetOrientationAndPosition hook
        auto get_orientation_and_position_fn = hmd_lib->find_function(L"GetOrientationAndPosition");

        if (get_orientation_and_position_fn != nullptr) {
            auto& native_fn_get_oap = get_orientation_and_position_fn->get_native_function();

            if (native_fn_get_oap != nullptr && !IsBadReadPtr(native_fn_get_oap, sizeof(void*))) {
                g_hook->m_native_get_oap_hook = std::make_unique<PointerHook>((void**)&native_fn_get_oap, get_orientation_and_position_native);
                SPDLOG_INFO("Hooked GetOrientationAndPosition native function");
            } else {
                SPDLOG_ERROR("Failed to hook GetOrientationAndPosition native function");
            }
        } else {
            SPDLOG_ERROR("Failed to find GetOrientationAndPosition native function");
        }

        // IsHeadMountedDisplayEnabled hook
        auto is_head_mounted_display_enabled_fn = hmd_lib->find_function(L"IsHeadMountedDisplayEnabled");

        if (is_head_mounted_display_enabled_fn != nullptr) {
            auto& native_fn_is_hmd_enabled = is_head_mounted_display_enabled_fn->get_native_function();

            if (native_fn_is_hmd_enabled != nullptr && !IsBadReadPtr(native_fn_is_hmd_enabled, sizeof(void*))) {
                g_hook->m_native_is_hmd_enabled_hook = std::make_unique<PointerHook>((void**)&native_fn_is_hmd_enabled, is_head_mounted_display_enabled_native);
                SPDLOG_INFO("Hooked IsHeadMountedDisplayEnabled native function");
            } else {
                SPDLOG_ERROR("Failed to hook IsHeadMountedDisplayEnabled native function");
            }
        } else {
            SPDLOG_ERROR("Failed to find IsHeadMountedDisplayEnabled native function");
        }
    } catch(...) {
        SPDLOG_ERROR("Failed to hook native functions due to exception");
    }

    if (!vr->is_any_aim_method_active()) {
        // Only allow this to return true if BP functions are the ones calling it
        // Like GetOrientationAndPosition
        return g_hook->is_within_valid_head_tracking_allowed_code();
    }

    if (!g_hook->m_process_view_rotation_hook && !g_hook->m_attempted_hook_view_rotation) {
        const auto return_address = (uintptr_t)_ReturnAddress();

        // <= 4.25 doesn't have IsHeadTrackingAllowedForWorld
        if (g_hook->m_is_leq_4_25) {
            g_hook->analyze_head_tracking_allowed(return_address);
        }
    }
    

    return !g_hook->m_process_view_rotation_hook || g_hook->is_within_valid_head_tracking_allowed_code();
}

bool IXRTrackingSystemHook::is_head_tracking_allowed_for_world(sdk::IXRTrackingSystem*, void*) {
    SPDLOG_INFO_ONCE("is_head_tracking_allowed_for_world {:x}", (uintptr_t)_ReturnAddress());

    auto& vr = VR::get();

    if (is_daysgone_executable() && vr->is_any_aim_method_active()) {
        return false;
    }

    if (is_direct_aim_compatibility_active()) {
        return false;
    }

    if (!vr->is_hmd_active() || !vr->is_any_aim_method_active()) {
        return false;
    }

    if (!g_hook->m_process_view_rotation_hook && !g_hook->m_attempted_hook_view_rotation) {
        const auto return_address = (uintptr_t)_ReturnAddress();
        g_hook->analyze_head_tracking_allowed(return_address);

        return true;
    }

    /*(if (!VR::get()->is_hmd_active()) {
        return false;
    }

    return true;*/

    return false;
}

IXRTrackingSystemHook::SharedPtr* IXRTrackingSystemHook::get_xr_camera(sdk::IXRTrackingSystem*, IXRTrackingSystemHook::SharedPtr* out, size_t device_id) {
    SPDLOG_INFO_ONCE("get_xr_camera {:x}", (uintptr_t)_ReturnAddress());

    if (out != nullptr) {
        *out = g_hook->m_xr_camera_shared;
    }

    if (VR::get()->is_hmd_active() && !g_hook->m_process_view_rotation_hook && !g_hook->m_attempted_hook_view_rotation) {
        const auto return_address = (uintptr_t)_ReturnAddress();
        ++detail::total_times_funcs_called;

        std::scoped_lock _{detail::return_address_to_functions_mutex};

        auto it = detail::return_address_to_functions.find(return_address);

        if (it == detail::return_address_to_functions.end()) {
            const auto vfunc = utility::find_virtual_function_start(return_address);

            if (vfunc) {
                detail::return_address_to_functions[return_address] = *vfunc;
            } else {
                detail::return_address_to_functions[return_address] = 0;
            }

            it = detail::return_address_to_functions.find(return_address);
        }

        if (it->second != 0) {
            auto& func = detail::functions[it->second];
            func.calls_xr_camera = true;
        }
    }

    return out;
}

void IXRTrackingSystemHook::get_motion_controller_data(sdk::IXRTrackingSystem*, void* world, uint32_t hand, void* motion_controller_data) {
    SPDLOG_INFO_ONCE("get_motion_controller_data {:x}", (uintptr_t)_ReturnAddress());

    const auto e_hand = (ue::EControllerHand)hand;
    const auto vr = VR::get();
    const auto world_scale = vr->get_world_to_meters();

    auto rotation_offset = vr->get_rotation_offset();

    if (vr->is_decoupled_pitch_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);
        rotation_offset = glm::normalize(pre_flat_pitch * vr->get_rotation_offset());
    }

    static const auto mc_data_struct = sdk::FUObjectArray::get() != nullptr ? (sdk::UScriptStruct*)sdk::find_uobject(L"ScriptStruct /Script/HeadMountedDisplay.XRMotionControllerData") : nullptr;

    const auto aim_transform = e_hand == ue::EControllerHand::Left ? vr->get_aim_transform(vr->get_left_controller_index()) : vr->get_aim_transform(vr->get_right_controller_index());
    const auto grip_transform = e_hand == ue::EControllerHand::Left ? vr->get_grip_transform(vr->get_left_controller_index()) : vr->get_grip_transform(vr->get_right_controller_index());

    const auto aim_position = rotation_offset * glm::vec3{aim_transform[3] - vr->get_standing_origin()};
    const auto aim_rotation = glm::normalize(rotation_offset * glm::quat{aim_transform});
    const auto grip_position = rotation_offset * glm::vec3{grip_transform[3] - vr->get_standing_origin()};
    const auto grip_rotation = glm::normalize(rotation_offset * glm::quat{grip_transform});

    const auto final_aim_position = utility::math::glm_to_ue4(aim_position * world_scale);
    const auto final_aim_rotation = utility::math::glm_to_ue4(aim_rotation);
    const auto final_grip_position = utility::math::glm_to_ue4(grip_position * world_scale);
    const auto final_grip_rotation = utility::math::glm_to_ue4(grip_rotation);

    if (mc_data_struct == nullptr) {
        const auto data = (ue4_27::FXRMotionControllerData*)motion_controller_data;
        data->bValid = true;
        data->GripRotation = { final_grip_rotation.x, final_grip_rotation.y, final_grip_rotation.z, final_grip_rotation.w };
        data->GripPosition = final_grip_position;
        data->AimRotation = { final_aim_rotation.x, final_aim_rotation.y, final_aim_rotation.z, final_aim_rotation.w };
        data->AimPosition = final_aim_position;
    } else {
        const auto bValid_prop = mc_data_struct->find_property(L"bValid");
        const auto GripRotation_prop = mc_data_struct->find_property(L"GripRotation");
        const auto GripPosition_prop = mc_data_struct->find_property(L"GripPosition");
        const auto AimRotation_prop = mc_data_struct->find_property(L"AimRotation");
        const auto AimPosition_prop = mc_data_struct->find_property(L"AimPosition");
        const auto is_ue5 = g_hook->m_stereo_hook->has_double_precision();

        if (bValid_prop != nullptr) {
            *bValid_prop->get_data<bool>(motion_controller_data) = true;
        }

        if (GripRotation_prop != nullptr) {
            if (is_ue5) {
                *GripRotation_prop->get_data<glm::vec<4, double>>(motion_controller_data) = { final_grip_rotation.x, final_grip_rotation.y, final_grip_rotation.z, final_grip_rotation.w };
            } else {
                *GripRotation_prop->get_data<glm::vec<4, float>>(motion_controller_data) = { final_grip_rotation.x, final_grip_rotation.y, final_grip_rotation.z, final_grip_rotation.w };
            }
        }

        if (GripPosition_prop != nullptr) {
            if (is_ue5) {
                *GripPosition_prop->get_data<glm::vec<3, double>>(motion_controller_data) = final_grip_position;
            } else {
                *GripPosition_prop->get_data<glm::vec<3, float>>(motion_controller_data) = final_grip_position;
            }
        }

        if (AimRotation_prop != nullptr) {
            if (is_ue5) {
                *AimRotation_prop->get_data<glm::vec<4, double>>(motion_controller_data) = { final_aim_rotation.x, final_aim_rotation.y, final_aim_rotation.z, final_aim_rotation.w };
            } else {
                *AimRotation_prop->get_data<glm::vec<4, float>>(motion_controller_data) = { final_aim_rotation.x, final_aim_rotation.y, final_aim_rotation.z, final_aim_rotation.w };
            }
        }

        if (AimPosition_prop != nullptr) {
            if (is_ue5) {
                *AimPosition_prop->get_data<glm::vec<3, double>>(motion_controller_data) = final_aim_position;
            } else {
                *AimPosition_prop->get_data<glm::vec<3, float>>(motion_controller_data) = final_aim_position;
            }
        }
    }
}

void IXRTrackingSystemHook::get_motion_controller_state(
    sdk::IXRTrackingSystem*, void* world, uint8_t space_type, uint8_t hand, uint8_t pose_type, void* motion_controller_state) {
    SPDLOG_INFO_ONCE("get_motion_controller_state {:x}", (uintptr_t)_ReturnAddress());

    auto* data = (ue5_7::FXRMotionControllerState*)motion_controller_state;
    if (!detail::can_write(data)) {
        SPDLOG_WARN_ONCE("get_motion_controller_state received an invalid output pointer");
        return;
    }

    std::memset(data, 0, sizeof(*data));

    const auto e_hand = (ue::EControllerHand)hand;
    if (e_hand != ue::EControllerHand::Left && e_hand != ue::EControllerHand::Right) {
        data->Hand = e_hand;
        data->XRSpaceType = (ue::EXRSpaceType)space_type;
        data->XRControllerPoseType = (ue::EXRControllerPoseType)pose_type;
        data->TrackingStatus = ue::ETrackingStatus::NotTracked;
        return;
    }

    const auto vr = VR::get();
    if (vr == nullptr) {
        data->Hand = e_hand;
        data->XRSpaceType = (ue::EXRSpaceType)space_type;
        data->XRControllerPoseType = (ue::EXRControllerPoseType)pose_type;
        data->TrackingStatus = ue::ETrackingStatus::NotTracked;
        return;
    }

    const auto world_scale = vr->get_world_to_meters();

    auto rotation_offset = vr->get_rotation_offset();

    if (vr->is_decoupled_pitch_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);
        rotation_offset = glm::normalize(pre_flat_pitch * vr->get_rotation_offset());
    }

    const auto controller_index = e_hand == ue::EControllerHand::Left ? vr->get_left_controller_index() : vr->get_right_controller_index();
    const auto aim_transform = vr->get_aim_transform(controller_index);
    const auto grip_transform = vr->get_grip_transform(controller_index);

    const auto aim_position = rotation_offset * glm::vec3{aim_transform[3] - vr->get_standing_origin()};
    const auto aim_rotation = glm::normalize(rotation_offset * glm::quat{aim_transform});
    const auto grip_position = rotation_offset * glm::vec3{grip_transform[3] - vr->get_standing_origin()};
    const auto grip_rotation = glm::normalize(rotation_offset * glm::quat{grip_transform});

    if (!detail::finite_vec3(aim_position) || !detail::finite_quat(aim_rotation) ||
        !detail::finite_vec3(grip_position) || !detail::finite_quat(grip_rotation)) {
        data->Hand = e_hand;
        data->XRSpaceType = (ue::EXRSpaceType)space_type;
        data->XRControllerPoseType = (ue::EXRControllerPoseType)pose_type;
        data->TrackingStatus = ue::ETrackingStatus::NotTracked;
        return;
    }

    const auto final_aim_position = utility::math::glm_to_ue4(aim_position * world_scale);
    const auto final_aim_rotation = utility::math::glm_to_ue4(aim_rotation);
    const auto final_grip_position = utility::math::glm_to_ue4(grip_position * world_scale);
    const auto final_grip_rotation = utility::math::glm_to_ue4(grip_rotation);

    const auto e_pose_type = (ue::EXRControllerPoseType)pose_type;
    const bool use_grip_pose = e_pose_type == ue::EXRControllerPoseType::Grip || e_pose_type == ue::EXRControllerPoseType::Palm;
    const auto& controller_position = use_grip_pose ? final_grip_position : final_aim_position;
    const auto& controller_rotation = use_grip_pose ? final_grip_rotation : final_aim_rotation;

    data->bValid = true;
    data->XRSpaceType = (ue::EXRSpaceType)space_type;
    data->Hand = e_hand;
    data->TrackingStatus = ue::ETrackingStatus::Tracked;
    data->XRControllerPoseType = e_pose_type;
    data->ControllerLocation = { controller_position.x, controller_position.y, controller_position.z };
    data->ControllerRotation = { controller_rotation.x, controller_rotation.y, controller_rotation.z, controller_rotation.w };
    data->GripUnrealSpaceLocation = { final_grip_position.x, final_grip_position.y, final_grip_position.z };
    data->GripUnrealSpaceRotation = { final_grip_rotation.x, final_grip_rotation.y, final_grip_rotation.z, final_grip_rotation.w };
}

void IXRTrackingSystemHook::get_hmd_data(sdk::IXRTrackingSystem*, void* world, void* hmd_data) {
    SPDLOG_INFO_ONCE("get_hmd_data {:x}", (uintptr_t)_ReturnAddress());

    if (hmd_data == nullptr || !detail::is_writable_process_range((uintptr_t)hmd_data, 1)) {
        SPDLOG_WARN_ONCE("get_hmd_data received an invalid output pointer");
        return;
    }

    const auto& vr = VR::get();
    const auto world_scale = vr->get_world_to_meters();

    auto rotation_offset = vr->get_rotation_offset();

    if (vr->is_decoupled_pitch_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);
        rotation_offset = glm::normalize(pre_flat_pitch * vr->get_rotation_offset());
    }

    static const auto hmd_data_struct = sdk::FUObjectArray::get() != nullptr ? (sdk::UScriptStruct*)sdk::find_uobject(L"ScriptStruct /Script/HeadMountedDisplay.XRHMDData") : nullptr;

    const auto position = rotation_offset * glm::vec3{vr->get_position(vr->get_hmd_index()) - vr->get_standing_origin()};
    const auto rotation = glm::normalize(rotation_offset * glm::quat{vr->get_rotation(vr->get_hmd_index())});

    if (hmd_data_struct == nullptr) {
        const auto data = (ue4_27::FXRHMDData*)hmd_data;
        data->bValid = true;
        data->TrackingStatus = ue::ETrackingStatus::Tracked;
        data->Position = utility::math::glm_to_ue4(position * world_scale);

        const auto q = utility::math::glm_to_ue4(rotation);
        data->Rotation = { q.x, q.y, q.z, q.w };
    } else {
        const auto bValid_prop = hmd_data_struct->find_property(L"bValid");
        const auto TrackingStatus_prop = hmd_data_struct->find_property(L"TrackingStatus");
        const auto Position_prop = hmd_data_struct->find_property(L"Position");
        const auto Rotation_prop = hmd_data_struct->find_property(L"Rotation");
        const auto is_ue5 = g_hook->m_stereo_hook->has_double_precision();

        if (bValid_prop != nullptr) {
            *bValid_prop->get_data<bool>(hmd_data) = true;
        }

        if (TrackingStatus_prop != nullptr) {
            *TrackingStatus_prop->get_data<ue::ETrackingStatus>(hmd_data) = ue::ETrackingStatus::Tracked;
        }

        if (Position_prop != nullptr) {
            if (is_ue5) {
                *Position_prop->get_data<glm::vec<3, double>>(hmd_data) = utility::math::glm_to_ue4(position * world_scale);
            } else {
                *Position_prop->get_data<glm::vec<3, float>>(hmd_data) = utility::math::glm_to_ue4(position * world_scale);
            }
        }

        if (Rotation_prop != nullptr) {
            if (is_ue5) {
                const auto q = utility::math::glm_to_ue4(rotation);
                *Rotation_prop->get_data<glm::vec<4, double>>(hmd_data) = { q.x, q.y, q.z, q.w };
            } else {
                const auto q = utility::math::glm_to_ue4(rotation);
                *Rotation_prop->get_data<glm::vec<4, float>>(hmd_data) = { q.x, q.y, q.z, q.w };
            }
        }
    }
}

void IXRTrackingSystemHook::get_current_pose(sdk::IXRTrackingSystem*, int32_t device_id, Quat<float>* out_rot, glm::vec3* out_pos) {
    SPDLOG_INFO_ONCE("get_current_pose {:x}", (uintptr_t)_ReturnAddress());

    const auto is_ue5 = g_hook->m_stereo_hook->has_double_precision();
    const auto out_pos_size = is_ue5 ? sizeof(glm::vec<3, double>) : sizeof(glm::vec3);
    const auto out_rot_size = is_ue5 ? sizeof(glm::vec<4, double>) : sizeof(Quat<float>);
    if (out_pos == nullptr || out_rot == nullptr ||
        !detail::is_writable_process_range((uintptr_t)out_pos, out_pos_size) ||
        !detail::is_writable_process_range((uintptr_t)out_rot, out_rot_size)) {
        SPDLOG_WARN_ONCE("get_current_pose received invalid output pointers");
        return;
    }

    const auto& vr = VR::get();
    const auto world_scale = vr->get_world_to_meters();

    auto rotation_offset = vr->get_rotation_offset();

    if (vr->is_decoupled_pitch_enabled()) {
        const auto pre_flat_rotation = vr->get_pre_flattened_rotation();
        const auto pre_flat_pitch = utility::math::pitch_only(pre_flat_rotation);
        rotation_offset = glm::normalize(pre_flat_pitch * vr->get_rotation_offset());
    }

    switch (device_id) {
    // Todo: motion controllers? Don't know how BP can pass through a valid device id
    case 0: 
    default: {
        const auto position = rotation_offset * glm::vec3{vr->get_position(vr->get_hmd_index()) - vr->get_standing_origin()};
        const auto rotation = glm::normalize(rotation_offset * glm::quat{vr->get_rotation(vr->get_hmd_index())});

        if (!is_ue5) {
            *out_pos = utility::math::glm_to_ue4(position * world_scale);

            const auto q = utility::math::glm_to_ue4(rotation);
            *out_rot = { q.x, q.y, q.z, q.w };
        } else {
            *(glm::vec<3, double>*)out_pos = utility::math::glm_to_ue4(position * world_scale);

            const auto q = utility::math::glm_to_ue4(rotation);
            *(glm::vec<4, double>*)out_rot = { q.x, q.y, q.z, q.w };
        }

        break;
    }
    }
}

enum ECustomSystemFlags : int32_t {
    SYSTEMFLAG_NONE = 0,
    SYSTEMFLAG_HMD_ACTIVE = 1 << 0,
    SYSTEMFLAG_DECOUPLED_PITCH = 1 << 1,
    SYSTEMFLAG_OPENXR = 1 << 2,
    SYSTEMFLAG_OPENVR = 1 << 3,
    SYSTEMFLAG_MOTION_CONTROLLERS_ACTIVE = 1 << 4,
    SYSTEMFLAG_LEFT_THUMBREST_ACTIVE = 1 << 5,
    SYSTEMFLAG_RIGHT_THUMBREST_ACTIVE = 1 << 6,
    SYSTEMFLAG_GAME_AIMING_MODE = 1 << 7,
    SYSTEMFLAG_HEAD_AIMING_MODE = 1 << 8,
    SYSTEMFLAG_LEFT_CONTROLLER_AIMING_MODE = 1 << 9,
    SYSTEMFLAG_RIGHT_CONTROLLER_AIMING_MODE = 1 << 10,
    SYSTEMFLAG_TWO_HANDED_LEFT_AIMING_MODE = 1 << 11,
    SYSTEMFLAG_TWO_HANDED_RIGHT_AIMING_MODE = 1 << 12,
};

int32_t IXRTrackingSystemHook::get_xr_system_flags(sdk::IXRTrackingSystem* system) {
    SPDLOG_INFO_ONCE("get_xr_system_flags {:x}", (uintptr_t)_ReturnAddress());

    const auto& vr = VR::get();

    if (!vr->is_hmd_active()) {
        return ECustomSystemFlags::SYSTEMFLAG_NONE;
    }

    int32_t out = ECustomSystemFlags::SYSTEMFLAG_HMD_ACTIVE;

    if (vr->is_decoupled_pitch_enabled()) {
        out |= ECustomSystemFlags::SYSTEMFLAG_DECOUPLED_PITCH;
    }

    if (vr->is_using_controllers()) {
        out |= ECustomSystemFlags::SYSTEMFLAG_MOTION_CONTROLLERS_ACTIVE;
    }

    const auto runtime = vr->get_runtime();

    if (runtime->is_openvr()) {
        out |= ECustomSystemFlags::SYSTEMFLAG_OPENVR;
    } else if (runtime->is_openxr()) {
        out |= ECustomSystemFlags::SYSTEMFLAG_OPENXR;
    }

    const auto left_thumbrest_handle = vr->get_action_handle(VR::s_action_thumbrest_touch_left);
    const auto right_thumbrest_handle = vr->get_action_handle(VR::s_action_thumbrest_touch_right);

    if (vr->is_action_active_any_joystick(left_thumbrest_handle)) {
        out |= ECustomSystemFlags::SYSTEMFLAG_LEFT_THUMBREST_ACTIVE;
    }

    if (vr->is_action_active_any_joystick(right_thumbrest_handle)) {
        out |= ECustomSystemFlags::SYSTEMFLAG_RIGHT_THUMBREST_ACTIVE;
    }

    if (!vr->is_any_aim_method_active()) {
        out |= ECustomSystemFlags::SYSTEMFLAG_GAME_AIMING_MODE;
    } else {
        const auto aim_method = vr->get_aim_method();

        if (aim_method == VR::AimMethod::HEAD) {
            out |= ECustomSystemFlags::SYSTEMFLAG_HEAD_AIMING_MODE;
        } else if (aim_method == VR::AimMethod::LEFT_CONTROLLER) {
            out |= ECustomSystemFlags::SYSTEMFLAG_LEFT_CONTROLLER_AIMING_MODE;
        } else if (aim_method == VR::AimMethod::RIGHT_CONTROLLER) {
            out |= ECustomSystemFlags::SYSTEMFLAG_RIGHT_CONTROLLER_AIMING_MODE;
        } else if (aim_method == VR::AimMethod::TWO_HANDED_LEFT) {
            out |= ECustomSystemFlags::SYSTEMFLAG_TWO_HANDED_LEFT_AIMING_MODE;
        } else if (aim_method == VR::AimMethod::TWO_HANDED_RIGHT) {
            out |= ECustomSystemFlags::SYSTEMFLAG_TWO_HANDED_RIGHT_AIMING_MODE;
        }
    }

    return out;
}

void* IXRTrackingSystemHook::get_audio_listener_offset(sdk::IXRTrackingSystem*, void* a2, void* a3) {
    SPDLOG_INFO_ONCE("get_audio_listener_offset {:x}", (uintptr_t)_ReturnAddress());

    static bool is_a2_stack = !IsBadReadPtr(a2, sizeof(void*));

    if (is_a2_stack) {
        float* foffset = (float*)a2;
        double* doffset = (double*)a2;

        if (g_hook->m_stereo_hook->has_double_precision()) {
            doffset[0] = 0.0;
            doffset[1] = 0.0;
            doffset[2] = 0.0;
        } else {
            foffset[0] = 0.0f;
            foffset[1] = 0.0f;
            foffset[2] = 0.0f;
        }

        return a2;
    }

    float* foffset = (float*)a3;
    double* doffset = (double*)a3;

    if (g_hook->m_stereo_hook->has_double_precision()) {
        doffset[0] = 0.0;
        doffset[1] = 0.0;
        doffset[2] = 0.0;
    } else {
        foffset[0] = 0.0f;
        foffset[1] = 0.0f;
        foffset[2] = 0.0f;
    }

    return a3;
}

// Returns quaternion
void* IXRTrackingSystemHook::get_base_orientation(sdk::IXRTrackingSystem*, void* a2) {
    SPDLOG_INFO_ONCE("get_base_orientation {:x}", (uintptr_t)_ReturnAddress());

    if (g_hook->m_stereo_hook->has_double_precision()) {
        Quat<double>* out = (Quat<double>*)a2;

        out->x = 0.0;
        out->y = 0.0;
        out->z = 0.0;
        out->w = 1.0;
    } else {
        Quat<float>* out = (Quat<float>*)a2;

        out->x = 0.0f;
        out->y = 0.0f;
        out->z = 0.0f;
        out->w = 1.0f;
    }

    return a2;
}

// Returns vec3
void* IXRTrackingSystemHook::get_base_position(sdk::IXRTrackingSystem*, void* a2) {
    SPDLOG_INFO_ONCE("get_base_position {:x}", (uintptr_t)_ReturnAddress());

    if (g_hook->m_stereo_hook->has_double_precision()) {
        double* out = (double*)a2;

        out[0] = 0.0;
        out[1] = 0.0;
        out[2] = 0.0;
    } else {
        float* out = (float*)a2;

        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
    }

    return a2;
}

// Returns Rotator
void* IXRTrackingSystemHook::get_base_rotation(sdk::IXRTrackingSystem*, void* a2) {
    SPDLOG_INFO_ONCE("get_base_rotation {:x}", (uintptr_t)_ReturnAddress());

    if (g_hook->m_stereo_hook->has_double_precision()) {
        Rotator<double>* out = (Rotator<double>*)a2;

        out->pitch = 0.0;
        out->yaw = 0.0;
        out->roll = 0.0;
    } else {
        Rotator<float>* out = (Rotator<float>*)a2;

        out->pitch = 0.0f;
        out->yaw = 0.0f;
        out->roll = 0.0f;
    }

    return a2;
}

void* IXRTrackingSystemHook::reset_orientation_and_position(sdk::IXRTrackingSystem*, float yaw) {
    SPDLOG_INFO_ONCE("reset_orientation_and_position {:x}", (uintptr_t)_ReturnAddress());

    auto& vr = VR::get();
    vr->set_standing_origin(vr->get_position(vr->get_hmd_index()));
    vr->recenter_view();

    return nullptr;
}

void* IXRTrackingSystemHook::reset_orientation(sdk::IXRTrackingSystem*, float yaw) {
    SPDLOG_INFO_ONCE("reset_orientation {:x}", (uintptr_t)_ReturnAddress());

    auto& vr = VR::get();
    vr->recenter_view();

    return nullptr;
}

void* IXRTrackingSystemHook::reset_position(sdk::IXRTrackingSystem*) {
    SPDLOG_INFO_ONCE("reset_position {:x}", (uintptr_t)_ReturnAddress());

    auto& vr = VR::get();
    vr->set_standing_origin(vr->get_position(vr->get_hmd_index()));

    return nullptr;
}

bool IXRTrackingSystemHook::is_hmd_connected(sdk::IHeadMountedDisplay*) {
    SPDLOG_INFO_ONCE("is_hmd_connected {:x}", (uintptr_t)_ReturnAddress());

    return VR::get()->is_hmd_active();
}

int32_t* IXRTrackingSystemHook::get_ideal_debug_canvas_render_target_size(sdk::IHeadMountedDisplay*, int32_t* out) {
    const auto return_address = (uintptr_t)_ReturnAddress();
    SPDLOG_INFO_ONCE("get_ideal_debug_canvas_render_target_size {:x}", return_address);

    if (out != nullptr) {
        // Verify first that this function dereferences RAX at some point afterwards
        static std::mutex mtx{};
        static std::unordered_set<uintptr_t> valid_return_addresses{};
        static std::unordered_set<uintptr_t> invalid_return_addresses{};

        std::scoped_lock _{mtx};

        if (invalid_return_addresses.contains(return_address)) {
            return nullptr;
        }

        if (valid_return_addresses.contains(return_address)) {     
            // This is what the engine does... I guess? I don't see any VR plugins implementing this function
            // look into it later to see if it's even useful
            out[0] = 1024;
            out[1] = 1024;

            return out;
        }

        // Setup the emulator. Set RAX to magic value. If RAX changes at any point, abort.
        const auto module_within = utility::get_module_within(return_address);

        if (!module_within) {
            SPDLOG_ERROR("get_ideal_debug_canvas_render_target_size: invalid module within");
            invalid_return_addresses.insert(return_address);

            return nullptr;
        }

        static auto check_ix = [](const INSTRUX& ix) {
            for (auto i = 0; i < ix.OperandsCount; ++i) {
                const auto& op = ix.Operands[i];

                if (op.Type != ND_OPERAND_TYPE::ND_OP_MEM) {
                    continue;
                }

                if (op.Info.Memory.HasBase && op.Info.Memory.Base == NDR_RAX) {
                    SPDLOG_INFO("Found dereference of RAX");
                    return true;
                }
            }

            return false;
        };

        const auto retdecode = utility::decode_one((uint8_t*)return_address);

        if (!retdecode) {
            SPDLOG_ERROR("get_ideal_debug_canvas_render_target_size: invalid retdecode");
            invalid_return_addresses.insert(return_address);

            return nullptr;
        }

        bool is_valid = check_ix(*retdecode);

        if (!is_valid) {
            utility::ShemuContext base_context{*module_within};

            base_context.ctx->Registers.RegRip = return_address;
            base_context.ctx->Registers.RegRax = 0xdeadbeef;
            base_context.ctx->MemThreshold = 10;

            utility::emulate(*module_within, return_address, 10, base_context, [&is_valid](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                if (check_ix(ctx.ctx->ctx->Instruction) || check_ix(ctx.next.ix)) {
                    is_valid = true;
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.next.ix.BranchInfo.IsBranch) {
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.ctx->ctx->Registers.RegRax != 0xdeadbeef) {
                    return utility::ExhaustionResult::BREAK;
                }

                if (ctx.next.writes_to_memory) {
                    return utility::ExhaustionResult::STEP_OVER;
                }

                return utility::ExhaustionResult::CONTINUE;
            });
        }

        if (!is_valid) {
            SPDLOG_ERROR("get_ideal_debug_canvas_render_target_size: invalid emulation result");
            invalid_return_addresses.insert(return_address);
            return nullptr;
        }

        valid_return_addresses.insert(return_address);

        // This is what the engine does... I guess? I don't see any VR plugins implementing this function
        // look into it later to see if it's even useful
        out[0] = 1024;
        out[1] = 1024;

        return out;
    }

    return nullptr;
}

IXRTrackingSystemHook::SharedPtr* IXRTrackingSystemHook::get_view_extension(sdk::IHeadMountedDisplay*, SharedPtr* out) {
    SPDLOG_INFO_ONCE("get_view_extension {:x}", (uintptr_t)_ReturnAddress());

    if (out != nullptr) {
        *out = g_hook->m_view_extension_shared;
    }

    return out;
}

void IXRTrackingSystemHook::apply_hmd_rotation(sdk::IXRCamera*, sdk::APlayerController* player_controller, Rotator<float>* rot) {
    SPDLOG_INFO_ONCE("apply_hmd_rotation {:x}", (uintptr_t)_ReturnAddress());

    if (is_daysgone_executable() && VR::get()->is_any_aim_method_active()) {
        return;
    }

    if (VR::get()->is_hmd_active() && !g_hook->m_process_view_rotation_hook && !g_hook->m_attempted_hook_view_rotation) {
        const auto return_address = (uintptr_t)_ReturnAddress();
        ++detail::total_times_funcs_called;

        std::scoped_lock _{detail::return_address_to_functions_mutex};

        auto it = detail::return_address_to_functions.find(return_address);

        if (it == detail::return_address_to_functions.end()) {
            const auto vfunc = utility::find_virtual_function_start(return_address);

            if (vfunc) {
                detail::return_address_to_functions[return_address] = *vfunc;
            } else {
                detail::return_address_to_functions[return_address] = 0;
            }

            it = detail::return_address_to_functions.find(return_address);
        }

        if (it->second != 0) {
            auto& func = detail::functions[it->second];
            func.calls_apply_hmd_rotation = true;
        }

        g_hook->update_view_rotation(player_controller, rot);
    }

    /*if (g_hook->m_stereo_hook == nullptr) {
        return;
    }

    if (!VR::get()->is_hmd_active()) {
        return;
    }

    if (g_hook->m_stereo_hook->has_double_precision()) {
        *(Rotator<double>*)rot = g_hook->m_stereo_hook->m_last_rotation_double;
    } else {
        *rot = g_hook->m_stereo_hook->m_last_rotation;
    }

    VR::get()->recenter_view();*/
}

bool IXRTrackingSystemHook::update_player_camera(sdk::IXRCamera*, Quat<float>* rel_rot, glm::vec3* rel_pos) {
    SPDLOG_INFO_ONCE("update_player_camera {:x}", (uintptr_t)_ReturnAddress());

    if (is_daysgone_executable() && VR::get()->is_any_aim_method_active()) {
        return false;
    }

    if (VR::get()->is_hmd_active() && !g_hook->m_process_view_rotation_hook && !g_hook->m_attempted_hook_view_rotation) {
        ++detail::total_times_funcs_called;

        std::scoped_lock _{detail::return_address_to_functions_mutex};

        const auto return_address = (uintptr_t)_ReturnAddress();
        auto it = detail::return_address_to_functions.find(return_address);

        if (it == detail::return_address_to_functions.end()) {
            const auto vfunc = utility::find_virtual_function_start(return_address);

            if (vfunc) {
                detail::return_address_to_functions[return_address] = *vfunc;
            } else {
                detail::return_address_to_functions[return_address] = 0;
            }

            it = detail::return_address_to_functions.find(return_address);
        }

        if (it->second != 0) {
            auto& func = detail::functions[it->second];
            func.calls_update_player_camera = true;
        }
    }

    if (!g_hook->m_relative_transform_corrected) {
        g_hook->m_relative_transform_corrected = true;

        const auto return_address = (uintptr_t)_ReturnAddress();
        const auto module_within = utility::get_module_within(return_address);

        if (module_within) {
            // We need to emulate from the return address and find the first call
            // We need to nop out this call because it modifies the relative transform
            // causing the player to turn into a midget in some games
            // and also in some games causes strange rotation issues
            utility::ShemuContext ctx{*module_within};

            ctx.ctx->Registers.RegRip = return_address;
            ctx.ctx->Registers.RegRax = 1;
            ctx.ctx->MemThreshold = 100;

            utility::emulate(*module_within, return_address, 100, ctx, [](const utility::ShemuContextExtended& ctx) -> utility::ExhaustionResult {
                if (ctx.next.writes_to_memory) {
                    spdlog::info("Skipping memory write at {:x}", ctx.ctx->ctx->Registers.RegRip);
                    return utility::ExhaustionResult::STEP_OVER;
                }
    
                if (std::string_view{ctx.next.ix.Mnemonic}.starts_with("CALL")) {
                    SPDLOG_INFO("Creating nop patch at {:x}", ctx.ctx->ctx->Registers.RegRip);
                    static auto patch = Patch::create_nop(ctx.ctx->ctx->Registers.RegRip, ctx.next.ix.Length);
                    return utility::ExhaustionResult::BREAK;
                }

                return utility::ExhaustionResult::CONTINUE;
            });
        } else {
            SPDLOG_WARN("Could not find module within {:x}", return_address);
        }
    }

    if (g_hook->m_stereo_hook->has_double_precision()) {
        *(Quat<double>*)rel_rot = { 0.0, 0.0, 0.0, 1.0};
        double* rel_pos_d = (double*)rel_pos;
        rel_pos_d[0] = 0.0;
        rel_pos_d[1] = 0.0;
        rel_pos_d[2] = 0.0;
    } else {
        *rel_rot = { 0.0f, 0.0f, 0.0f, 1.0f };
        *rel_pos = { 0.0f, 0.0f, 0.0f };
    }
    return true;
}

void* IXRTrackingSystemHook::process_view_rotation_analyzer(void* a1, size_t a2, size_t a3, size_t a4, size_t a5, size_t a6) {
    SPDLOG_INFO_ONCE("process_view_rotation_analyzer {:x}", (uintptr_t)_ReturnAddress());

    std::scoped_lock _{detail::return_address_to_functions_mutex};

    auto call_orig = [&]() {
        return g_hook->m_process_view_rotation_hook.call<void*>(a1, a2, a3, a4, a5, a6);
    };

    const auto result = call_orig();

    // Not the exact stack pointer but at least some memory pointing to the stack
    // We need this to check whether the arguments reside on the stack.
    // If they don't, then this is not the correct function and we need to analyze the next one.
    const auto stack_pointer = (intptr_t)_AddressOfReturnAddress();

    // Check if a3 and a4 are on the stack, and are not equal to eachother
    if (std::abs((intptr_t)a3 - stack_pointer) > 0x1000 || std::abs((intptr_t)a4 - stack_pointer) > 0x1000 || a3 == a4) {
        SPDLOG_ERROR("Function we hooked for ProcessViewRotation is not the correct one. Analyzing next function.");

        const auto function_addr = g_hook->m_process_view_rotation_hook.target_address();
        detail::functions[function_addr].process_view_rotation_analysis_failed = true;

        g_hook->m_process_view_rotation_hook.reset();
        g_hook->m_attempted_hook_view_rotation = false;
    } else {
        SPDLOG_INFO("Found correct function for ProcessViewRotation. Hooking it.");
        const auto target = g_hook->m_process_view_rotation_hook.target();
        g_hook->m_process_view_rotation_hook.reset();
        //g_hook->m_process_view_rotation_hook = std::make_unique<PointerHook>((void**)g_hook->m_addr_of_process_view_rotation_ptr, &IXRTrackingSystemHook::process_view_rotation);
        g_hook->m_process_view_rotation_hook = safetyhook::create_inline(target, &IXRTrackingSystemHook::process_view_rotation);
    }

    return result;
}

void IXRTrackingSystemHook::process_view_rotation(
    sdk::APlayerCameraManager* pcm, float delta_time, Rotator<float>* rot, Rotator<float>* delta_rot) {
    SPDLOG_INFO_ONCE("process_view_rotation {:x}", (uintptr_t)_ReturnAddress());

    auto call_orig = [&]() {
        g_hook->m_process_view_rotation_hook.call<void>(pcm, delta_time, rot, delta_rot);
    };

    auto& vr = VR::get();

    if (is_daysgone_executable() && vr->is_any_aim_method_active()) {
        call_orig();
        return;
    }

    if (!vr->is_hmd_active() || !vr->is_any_aim_method_active()) {
        call_orig();
        return;
    }

    if (vr->is_controller_camera_conflict_guard_active()) {
        SPDLOG_INFO_ONCE("[ControllerCameraGuard] Bypassing ProcessViewRotation mutation");
        call_orig();
        return;
    }

    if (is_direct_aim_compatibility_active()) {
        // Some games crash or misbehave in camera-manager XR paths when we mutate the
        // PCM rotation directly. Keep the game path intact and drive control
        // rotation through the safer manual direct-aim fallback instead.
        call_orig();
        return;
    }

    //g_hook->pre_update_view_rotation(rot);

    call_orig();

    g_hook->update_view_rotation(pcm, rot);
}

void IXRTrackingSystemHook::pre_update_view_rotation(sdk::UObject* reference_obj, Rotator<float>* rot) {
    auto& vr = VR::get();

    if (m_stereo_hook->has_double_precision()) {
        //*(Rotator<double>*)rot = g_hook->m_stereo_hook->m_last_rotation_double;
        *(Rotator<double>*)rot = g_hook->m_last_view_rotation_double;
    } else {
        //*rot = g_hook->m_stereo_hook->m_last_rotation;
        *rot = g_hook->m_last_view_rotation;
    }
}

void IXRTrackingSystemHook::update_view_rotation(sdk::UObject* reference_obj, Rotator<float>* rot) {
    auto& vr = VR::get();

    if (is_daysgone_executable() && vr->is_any_aim_method_active()) {
        return;
    }

    // Double check that the player controller passed through here is the local player controller
    static bool had_detection_error = false;
    if (!is_payday3_aim_guard_enabled() && !is_deadzone_ue56_executable() && !is_ue4_14_through_4_17() &&
        !is_daysgone_executable() &&
        !had_detection_error && vr->is_aim_multiplayer_support_enabled()) try {
        if (reference_obj != nullptr && sdk::FUObjectArray::get() != nullptr) {
            const auto reference_obj_c = reference_obj->get_class();

            static const auto player_controller_class = sdk::APlayerController::static_class();
            static const auto player_camera_manager_class = sdk::APlayerCameraManager::static_class();

            sdk::APlayerController* pc = nullptr;

            if (player_controller_class != nullptr && reference_obj_c != nullptr && reference_obj_c->is_a(player_controller_class)) {
                pc = (sdk::APlayerController*)reference_obj;
            } else if (player_camera_manager_class != nullptr && reference_obj_c != nullptr && reference_obj_c->is_a(player_camera_manager_class)) {
                const auto pcm = (sdk::APlayerCameraManager*)reference_obj;
                pc = pcm->get_owning_player_controller();
            }

            if (pc != nullptr && !pc->is_local_player_controller()) {
                return;
            }
        }
    } catch(...) {
        had_detection_error = true;
        SPDLOG_ERROR("[IXRTrackingSystemHook] Error detecting reference object, cannot support multiplayer");
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const auto delta_time = now - m_process_view_rotation_data.last_update;
    const auto delta_float = glm::min(std::chrono::duration_cast<std::chrono::duration<float>>(delta_time).count(), 0.1f);
    m_process_view_rotation_data.last_update = now;
    m_process_view_rotation_data.was_called = true;

    if (!vr->is_hmd_active() || !vr->is_any_aim_method_active()) {
        return;
    }

    if (vr->is_controller_camera_conflict_guard_active()) {
        return;
    }

    const auto has_double = g_hook->m_stereo_hook->has_double_precision();
    const auto rot_size = has_double ? sizeof(Rotator<double>) : sizeof(Rotator<float>);
    if (!detail::is_writable_process_range((uintptr_t)rot, rot_size)) {
        SPDLOG_WARN_ONCE("[IXRTrackingSystemHook] update_view_rotation received an invalid rotation pointer");
        return;
    }

    if (!vr->is_decoupled_pitch_enabled()) {
        m_process_view_rotation_data.auto_enabled_decoupled_pitch = true;
        vr->set_decoupled_pitch(true);
    }

    auto rot_d = (Rotator<double>*)rot;
    const glm::vec3 input_euler = has_double
        ? glm::vec3{(float)rot_d->pitch, (float)rot_d->yaw, (float)rot_d->roll}
        : glm::vec3{rot->pitch, rot->yaw, rot->roll};

    if (!detail::finite_euler(input_euler)) {
        SPDLOG_WARN_ONCE("[IXRTrackingSystemHook] update_view_rotation skipped non-finite input rotation");
        return;
    }
    
    const auto view_mat_inverse =
    has_double ?
        glm::yawPitchRoll(
            glm::radians((float)-rot_d->yaw),
            glm::radians((float)rot_d->pitch),
            glm::radians((float)-rot_d->roll))
        : glm::yawPitchRoll(
            glm::radians(-rot->yaw),
            glm::radians(rot->pitch),
            glm::radians(-rot->roll));

    const auto view_quat_inverse = glm::quat {
        view_mat_inverse
    };

    auto vqi_norm = glm::normalize(view_quat_inverse);

    // Decoupled Pitch
    if (vr->is_decoupled_pitch_enabled()) {
        //vr->set_pre_flattened_rotation(vqi_norm);
        vqi_norm = utility::math::flatten(vqi_norm);
    }

    const auto wants_controller = vr->is_controller_aim_enabled() && vr->is_using_controllers();
    const auto rotation_offset = vr->get_rotation_offset();
    const auto og_hmd_pos = vr->get_position(0);
    const auto og_hmd_rot = glm::quat{vr->get_rotation(0)};

    glm::vec3 euler{};

    if (wants_controller) {
        glm::vec3 right_controller_forward{};
        glm::vec3 og_controller_pos{};
        glm::quat og_controller_rot{};

        const auto aim_type = (VR::AimMethod)vr->get_aim_method();

        if (aim_type == VR::AimMethod::RIGHT_CONTROLLER || aim_type == VR::AimMethod::LEFT_CONTROLLER) {
            const auto controller_index = aim_type == VR::AimMethod::RIGHT_CONTROLLER ? vr->get_right_controller_index() : vr->get_left_controller_index();
            og_controller_rot = aim_type == VR::AimMethod::RIGHT_CONTROLLER
                ? glm::quat{vr->get_controller_rotation_with_offset(VRRuntime::Hand::RIGHT)}
                : glm::quat{vr->get_controller_rotation_with_offset(VRRuntime::Hand::LEFT)};
            og_controller_pos = glm::vec3{vr->get_aim_position(controller_index)};
            right_controller_forward = og_controller_rot * glm::vec3{0.0f, 0.0f, 1.0f};
        } else if (aim_type == VR::AimMethod::TWO_HANDED_RIGHT) { // two handed modes are for imitating rifle aiming
            const auto right_controller_index = vr->get_right_controller_index();
            const auto left_controller_index = vr->get_left_controller_index();
            const auto raw_delta = glm::vec3{vr->get_aim_position(left_controller_index) - vr->get_aim_position(right_controller_index)};
            const auto delta_len_sq = glm::dot(raw_delta, raw_delta);
            if (!detail::finite_vec3(raw_delta) || delta_len_sq <= 0.000001f) {
                return;
            }

            const auto pos_delta = glm::normalize(raw_delta);
            og_controller_rot = utility::math::to_quat(pos_delta);
            og_controller_pos = glm::vec3{vr->get_aim_position(right_controller_index)};
            right_controller_forward = og_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f};
        } else if (aim_type == VR::AimMethod::TWO_HANDED_LEFT) {
            const auto right_controller_index = vr->get_right_controller_index();
            const auto left_controller_index = vr->get_left_controller_index();
            const auto raw_delta = glm::vec3{vr->get_aim_position(right_controller_index) - vr->get_aim_position(left_controller_index)};
            const auto delta_len_sq = glm::dot(raw_delta, raw_delta);
            if (!detail::finite_vec3(raw_delta) || delta_len_sq <= 0.000001f) {
                return;
            }

            const auto pos_delta = glm::normalize(raw_delta);
            og_controller_rot = utility::math::to_quat(pos_delta);
            og_controller_pos = glm::vec3{vr->get_aim_position(left_controller_index)};
            right_controller_forward = og_controller_rot * glm::vec3{0.0f, 0.0f, -1.0f};
        }
        // We need to construct a sightline from the standing origin to the direction the controller is facing
        // This is so the camera will be facing a more correct direction
        // rather than the raw controller rotation
        const auto right_controller_end = og_controller_pos + (right_controller_forward * 1000.0f);
        const auto adjusted_forward_delta = right_controller_end - glm::vec3{vr->get_standing_origin()};
        const auto adjusted_forward_len_sq = glm::dot(adjusted_forward_delta, adjusted_forward_delta);
        if (!detail::finite_vec3(adjusted_forward_delta) || adjusted_forward_len_sq <= 0.000001f) {
            return;
        }

        const auto adjusted_forward = glm::normalize(adjusted_forward_delta);
        const auto target_forward = utility::math::to_quat(adjusted_forward);

        glm::quat right_controller_forward_rot{};

        if (vr->is_aim_interpolation_enabled()) {
            // quaternion distance between target_forward and last_aim_rot
            auto spherical_distance = glm::dot(target_forward, m_process_view_rotation_data.last_aim_rot);

            if (spherical_distance < 0.0f) {
                // we do this because we want to rotate the shortest distance
                spherical_distance = -spherical_distance;
            }

            right_controller_forward_rot = glm::slerp(m_process_view_rotation_data.last_aim_rot, target_forward, delta_float * vr->get_aim_speed() * spherical_distance);
        } else {
            right_controller_forward_rot = target_forward;
        }

        const auto wanted_rotation = glm::normalize(rotation_offset * right_controller_forward_rot);
        const auto new_rotation = glm::normalize(vqi_norm * wanted_rotation);
        euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

        vr->set_rotation_offset(glm::inverse(utility::math::flatten(right_controller_forward_rot)));

        m_process_view_rotation_data.last_aim_rot = right_controller_forward_rot;

        /*const auto previous_standing_origin = glm::vec3{vr->get_standing_origin()};
        static auto last_delta = glm::vec3{og_controller_pos};
        static auto last_rot_delta = utility::math::flatten(right_controller_forward_rot);
        const auto current_delta = glm::vec3{og_controller_pos};
        const auto current_rot_delta = utility::math::flatten(right_controller_forward_rot);
        const auto delta_delta = current_delta - last_delta;
        const auto rot_delta_delta = glm::normalize(current_rot_delta * glm::inverse(last_rot_delta));
        const auto new_standing_origin = previous_standing_origin + delta_delta;
        vr->set_standing_origin(glm::vec4{new_standing_origin.x, new_standing_origin.y, new_standing_origin.z, 1.0f});

        last_delta = current_delta;
        last_rot_delta = current_rot_delta;*/
    } else {
        const auto current_hmd_rotation = glm::normalize(rotation_offset * og_hmd_rot);
        const auto new_rotation = glm::normalize(vqi_norm * current_hmd_rotation);
        euler = glm::degrees(utility::math::euler_angles_from_steamvr(new_rotation));

        vr->recenter_view();
    }

    if (!detail::finite_euler(euler)) {
        SPDLOG_WARN_ONCE("[IXRTrackingSystemHook] update_view_rotation skipped non-finite output rotation");
        return;
    }

    if (has_double) {
        rot_d->pitch = euler.x;
        rot_d->yaw = euler.y;
        rot_d->roll = euler.z;
        g_hook->m_last_view_rotation_double = *rot_d;
    } else {
        rot->pitch = euler.x;
        rot->yaw = euler.y;
        rot->roll = euler.z;
        g_hook->m_last_view_rotation = *rot;
    }
}
