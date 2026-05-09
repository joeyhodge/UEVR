#define NOMINMAX

#include <fstream>
#include <cmath>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <vector>

#include <windows.h>
#include <dbt.h>

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <utility/Module.hpp>
#include <utility/Registry.hpp>
#include <utility/ScopeGuard.hpp>

#include <sdk/Globals.hpp>
#include <sdk/CVar.hpp>
#include <sdk/threading/GameThreadWorker.hpp>
#include <sdk/UGameplayStatics.hpp>
#include <sdk/APlayerController.hpp>
#include <sdk/APlayerCameraManager.hpp>
#include <sdk/UEngine.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/TArray.hpp>
#include <sdk/Utility.hpp>

#include <tracy/Tracy.hpp>

#include "Framework.hpp"
#include "frameworkConfig.hpp"

#include "utility/Logging.hpp"

#include "VR.hpp"
#include "UObjectHook.hpp"
#include "GameSpecific.hpp"

std::shared_ptr<VR>& VR::get() {
    //static std::shared_ptr<VR> instance = std::make_shared<VR>();
    return g_framework->vr();
}

VR::~VR() {
    stop_hitch_snapshot_writer();
}

bool VR::on_openxr_resolution_scale_changed(
    uint32_t old_width,
    uint32_t old_height,
    uint32_t new_width,
    uint32_t new_height) {
    bool ue57_invalidated = false;

    if (m_fake_stereo_hook != nullptr) {
        ue57_invalidated = m_fake_stereo_hook->invalidate_ue57_resolution_dependent_state(old_width, old_height, new_width, new_height);
    }

    struct OpenXRResolutionReconfigurePolicy {
        bool live_allowed{false};
        std::string version{"0.00"};
        const char* reason{"unknown_version"};
    };

    const auto legacy_live_policy = []() {
        static const auto result = []() {
            const auto disk_version = sdk::get_file_version_info();
            const auto str_version = utility::narrow(sdk::search_for_version(utility::get_executable()).value_or(L"0.00"));

            if (str_version != "0.00") {
                const auto live_allowed =
                    str_version.starts_with("4.27") ||
                    str_version.starts_with("5.3") ||
                    str_version.starts_with("5.4") ||
                    str_version.starts_with("5.5") ||
                    str_version.starts_with("5.6") ||
                    str_version.starts_with("5.7") ||
                    str_version.starts_with("5.8") ||
                    str_version.starts_with("5.9");

                return OpenXRResolutionReconfigurePolicy{
                    .live_allowed = live_allowed,
                    .version = str_version,
                    .reason = live_allowed ? "legacy_safe_band" : "blocked_ue50_52_or_unknown"
                };
            }

            const auto major = (disk_version.dwFileVersionMS >> 16) & 0xFFFF;
            const auto minor = disk_version.dwFileVersionMS & 0xFFFF;
            const auto live_allowed = (major == 4 && minor == 27) || (major == 5 && minor >= 3);
            std::ostringstream version{};
            version << "file_version_" << major << "." << minor;

            return OpenXRResolutionReconfigurePolicy{
                .live_allowed = live_allowed,
                .version = version.str(),
                .reason = live_allowed ? "legacy_safe_file_version_band" : "blocked_file_version"
            };
        }();

        return result;
    }();

    if (!ue57_invalidated && !legacy_live_policy.live_allowed) {
        SPDLOG_WARN(
            "[OpenXR] Live resolution-scale reconfigure is disabled for this engine path; version={} reason={} saved value will apply after reinject/restart [{}x{}]->[{}x{}]",
            legacy_live_policy.version,
            legacy_live_policy.reason,
            old_width,
            old_height,
            new_width,
            new_height);
        return false;
    }

    SPDLOG_INFO(
        "[OpenXR] Live resolution-scale reconfigure is allowed; version={} reason={} ue57_invalidated={} [{}x{}]->[{}x{}]",
        legacy_live_policy.version,
        ue57_invalidated ? "ue57_resolution_state_invalidated" : legacy_live_policy.reason,
        ue57_invalidated,
        old_width,
        old_height,
        new_width,
        new_height);

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->set_should_recreate_textures(true);
    }

    if (m_openxr != nullptr && get_runtime() != nullptr && get_runtime()->is_openxr()) {
        m_openxr->prepare_resolution_scale_reconfigure(
            ue57_invalidated ? "ue57_resolution_scale_reconfigure" : "legacy_resolution_scale_reconfigure");
    }

    reinitialize_renderer();
    return true;
}

namespace {
using json = nlohmann::json;

constexpr bool STALKER2_TRANSITION_OPENXR_DEFERS_ENABLED = false;

int64_t hitch_age_ms(std::chrono::steady_clock::time_point now, std::chrono::steady_clock::time_point then) {
    if (then.time_since_epoch().count() == 0) {
        return -1;
    }

    return std::chrono::duration_cast<std::chrono::milliseconds>(now - then).count();
}

int64_t steady_clock_ms(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::string hitch_timestamp_suffix() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &time);

    std::ostringstream out{};
    out << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return out.str();
}

bool is_stalker2_executable_cached() {
    static const bool is_stalker2 = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_stalker2_executable_path(*exe_path);
    }();

    return is_stalker2;
}

bool should_defer_stalker2_very_late_openxr_wait(const VRRuntime* runtime, bool is_d3d12) {
    if (runtime == nullptr || !is_d3d12 || !runtime->is_openxr() || !is_stalker2_executable_cached()) {
        return false;
    }

    // Stalker2 can stall for seconds if a VERY_LATE xrWaitFrame is held across
    // the UE5.1 gameplay-load/render handoff. After initial valid poses, let
    // the D3D12 submit path own wait/begin/end so the frame loop stays local to
    // the copy/submit that actually presents to the HMD.
    return runtime->got_first_sync && runtime->got_first_valid_poses;
}

struct GameFovResolver {
    int32_t read_camera_cache_offset{-1};
    int32_t camera_cache_offset{-1};
    int32_t last_frame_camera_cache_offset{-1};
    int32_t camera_cache_private_offset{-1};
    int32_t last_frame_camera_cache_private_offset{-1};
    int32_t pov_offset{-1};
    int32_t fov_offset{-1};
    int32_t location_offset{-1};
    int32_t rotation_offset{-1};
    int32_t default_fov_offset{-1};
    bool attempted{false};
    bool valid{false};
};

GameFovResolver g_game_fov_resolver{};

bool is_ue418_executable() {
    static const auto result = []() {
        const auto version = sdk::search_for_version(utility::get_executable()).value_or(L"");
        return version.starts_with(L"4.18");
    }();

    return result;
}

enum class ProSpiCameraPreset : int32_t {
    None = 0,
    GameplayBehindPlate,
    HomePlateWaistHigh,
    HomePlateWaistHighReverse,
    BehindPlateWideTelephoto,
    BehindPlateElevatedSweep,
    OpeningAerialTelephoto,
    TVBroadcast,
    PlateHighTelephoto,
    HomePlateOverheadTelephoto,
    CenterFieldTelephoto,
    CenterFieldHighTelephoto,
    OffsetCenterFieldTelephoto,
    DeepOutfieldTelephoto,
    HomePlateSkyAerial,
    UpperDeckTelephoto,
    UpperDeckHomeSkyTelephoto,
    ThirdBaseTelephoto,
    ThirdBaseRelayLow,
    ThirdBaseCornerLow,
    ThirdBaseWideTelephoto,
    FirstBaseTelephoto,
    FirstBaseWideTelephoto,
    FirstBaseCornerLow,
    FirstBaseInfieldLow,
    LowInfieldSideCloseUp,
    BackstopHighTelephoto,
    RightFieldCornerTelephoto,
    RightCenterFieldTelephoto,
    GenericTelephoto,
};

const char* get_prospi_camera_preset_name(ProSpiCameraPreset preset) {
    switch (preset) {
    case ProSpiCameraPreset::GameplayBehindPlate:
        return "ProSpi: Gameplay Behind Plate";
    case ProSpiCameraPreset::HomePlateWaistHigh:
        return "ProSpi: Home Plate Waist High";
    case ProSpiCameraPreset::HomePlateWaistHighReverse:
        return "ProSpi: Home Plate Waist High Reverse";
    case ProSpiCameraPreset::BehindPlateWideTelephoto:
        return "ProSpi: Behind Plate Wide Telephoto";
    case ProSpiCameraPreset::BehindPlateElevatedSweep:
        return "ProSpi: Behind Plate Elevated Sweep";
    case ProSpiCameraPreset::OpeningAerialTelephoto:
        return "ProSpi: Opening Aerial Telephoto";
    case ProSpiCameraPreset::TVBroadcast:
        return "ProSpi: TV Broadcast";
    case ProSpiCameraPreset::PlateHighTelephoto:
        return "ProSpi: High Plate Telephoto";
    case ProSpiCameraPreset::HomePlateOverheadTelephoto:
        return "ProSpi: Home Plate Overhead Telephoto";
    case ProSpiCameraPreset::CenterFieldTelephoto:
        return "ProSpi: Center Field Telephoto";
    case ProSpiCameraPreset::CenterFieldHighTelephoto:
        return "ProSpi: Center Field High Telephoto";
    case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
        return "ProSpi: Offset Center Field Telephoto";
    case ProSpiCameraPreset::DeepOutfieldTelephoto:
        return "ProSpi: Deep Outfield Telephoto";
    case ProSpiCameraPreset::HomePlateSkyAerial:
        return "ProSpi: Home Plate Sky Aerial";
    case ProSpiCameraPreset::UpperDeckTelephoto:
        return "ProSpi: Upper Deck Third Base Telephoto";
    case ProSpiCameraPreset::UpperDeckHomeSkyTelephoto:
        return "ProSpi: Upper Deck Home Sky Telephoto";
    case ProSpiCameraPreset::ThirdBaseTelephoto:
        return "ProSpi: Third Base Line Telephoto";
    case ProSpiCameraPreset::ThirdBaseRelayLow:
        return "ProSpi: Third Base Relay Low";
    case ProSpiCameraPreset::ThirdBaseCornerLow:
        return "ProSpi: Third Base Corner Low";
    case ProSpiCameraPreset::ThirdBaseWideTelephoto:
        return "ProSpi: Third Base Wide Telephoto";
    case ProSpiCameraPreset::FirstBaseTelephoto:
        return "ProSpi: First Base Line Telephoto";
    case ProSpiCameraPreset::FirstBaseWideTelephoto:
        return "ProSpi: First Base Wide Telephoto";
    case ProSpiCameraPreset::FirstBaseCornerLow:
        return "ProSpi: First Base Corner Low";
    case ProSpiCameraPreset::FirstBaseInfieldLow:
        return "ProSpi: First Base Infield Low";
    case ProSpiCameraPreset::LowInfieldSideCloseUp:
        return "ProSpi: Low Infield Side Close-Up";
    case ProSpiCameraPreset::BackstopHighTelephoto:
        return "ProSpi: Backstop High Telephoto";
    case ProSpiCameraPreset::RightFieldCornerTelephoto:
        return "ProSpi: Right Field Corner Telephoto";
    case ProSpiCameraPreset::RightCenterFieldTelephoto:
        return "ProSpi: Right Center Field Telephoto";
    case ProSpiCameraPreset::GenericTelephoto:
        return "ProSpi: Generic Telephoto";
    case ProSpiCameraPreset::None:
    default:
        return "None";
    }
}

bool is_specific_prospi_preset(ProSpiCameraPreset preset) {
    return preset != ProSpiCameraPreset::None && preset != ProSpiCameraPreset::GenericTelephoto;
}

float normalize_angle_delta(float a, float b) {
    auto delta = std::fmod(a - b, 360.0f);
    if (delta > 180.0f) {
        delta -= 360.0f;
    } else if (delta < -180.0f) {
        delta += 360.0f;
    }

    return std::abs(delta);
}

float get_prospi_sticky_location_tolerance(ProSpiCameraPreset preset) {
    switch (preset) {
    case ProSpiCameraPreset::OpeningAerialTelephoto:
        return 35000.0f;
    case ProSpiCameraPreset::TVBroadcast:
        return 2500.0f;
    case ProSpiCameraPreset::CenterFieldTelephoto:
    case ProSpiCameraPreset::CenterFieldHighTelephoto:
    case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
    case ProSpiCameraPreset::RightCenterFieldTelephoto:
        return 2500.0f;
    case ProSpiCameraPreset::HomePlateSkyAerial:
        return 2200.0f;
    case ProSpiCameraPreset::BehindPlateElevatedSweep:
        return 3200.0f;
    case ProSpiCameraPreset::ThirdBaseRelayLow:
    case ProSpiCameraPreset::ThirdBaseCornerLow:
    case ProSpiCameraPreset::FirstBaseCornerLow:
        return 1800.0f;
    case ProSpiCameraPreset::HomePlateOverheadTelephoto:
    case ProSpiCameraPreset::BackstopHighTelephoto:
        return 1800.0f;
    case ProSpiCameraPreset::GameplayBehindPlate:
    case ProSpiCameraPreset::HomePlateWaistHigh:
    case ProSpiCameraPreset::HomePlateWaistHighReverse:
    case ProSpiCameraPreset::LowInfieldSideCloseUp:
        return 1400.0f;
    default:
        return 2000.0f;
    }
}

bool should_keep_prospi_sticky_preset(
    ProSpiCameraPreset sticky_preset,
    const glm::vec3& sticky_location,
    const glm::vec3& sticky_rotation,
    float sticky_raw_fov,
    ProSpiCameraPreset candidate_preset,
    const glm::vec3& candidate_location,
    const glm::vec3& candidate_rotation,
    float candidate_raw_fov
) {
    if (sticky_preset == ProSpiCameraPreset::None) {
        return false;
    }

    if (candidate_preset == sticky_preset) {
        return true;
    }

    if (candidate_preset != ProSpiCameraPreset::None && candidate_preset != ProSpiCameraPreset::GenericTelephoto) {
        return false;
    }

    const auto location_delta = glm::distance(candidate_location, sticky_location);
    const auto yaw_delta = normalize_angle_delta(candidate_rotation.y, sticky_rotation.y);
    const auto pitch_delta = std::abs(candidate_rotation.x - sticky_rotation.x);
    const auto fov_delta = std::abs(candidate_raw_fov - sticky_raw_fov);

    return location_delta <= get_prospi_sticky_location_tolerance(sticky_preset) &&
           yaw_delta <= 22.5f &&
           pitch_delta <= 12.5f &&
           fov_delta <= 18.0f;
}

bool is_prospi_nontelephoto_preset(ProSpiCameraPreset preset) {
    switch (preset) {
    case ProSpiCameraPreset::GameplayBehindPlate:
    case ProSpiCameraPreset::HomePlateWaistHigh:
    case ProSpiCameraPreset::HomePlateWaistHighReverse:
    case ProSpiCameraPreset::BehindPlateWideTelephoto:
    case ProSpiCameraPreset::BehindPlateElevatedSweep:
    case ProSpiCameraPreset::FirstBaseInfieldLow:
    case ProSpiCameraPreset::LowInfieldSideCloseUp:
        return true;
    default:
        return false;
    }
}

bool is_prospi_line_telephoto_perf_preset(ProSpiCameraPreset preset) {
    switch (preset) {
    case ProSpiCameraPreset::ThirdBaseTelephoto:
    case ProSpiCameraPreset::ThirdBaseRelayLow:
    case ProSpiCameraPreset::ThirdBaseCornerLow:
    case ProSpiCameraPreset::ThirdBaseWideTelephoto:
    case ProSpiCameraPreset::FirstBaseTelephoto:
    case ProSpiCameraPreset::FirstBaseWideTelephoto:
    case ProSpiCameraPreset::FirstBaseCornerLow:
    case ProSpiCameraPreset::FirstBaseInfieldLow:
        return true;
    default:
        return false;
    }
}

bool is_prospi_line_telephoto_perf_location(const glm::vec3& location, const glm::vec3& rotation, float raw_fov) {
    if (raw_fov > 32.0f || std::abs(location.z) > 900.0f) {
        return false;
    }

    const auto near_first_or_third_line = std::abs(location.x) >= 1800.0f && std::abs(location.y) <= 2600.0f;
    const auto low_outfield_line = std::abs(location.x) <= 1200.0f && std::abs(location.y) >= 3000.0f;
    const auto looking_along_line = std::abs(rotation.y) >= 35.0f || std::abs(rotation.y) <= 15.0f;

    return (near_first_or_third_line || low_outfield_line) && looking_along_line;
}

bool is_prospi_executable() {
    static const bool is_prospi = []() {
        const auto module_path = utility::get_module_pathw(utility::get_executable());
        if (!module_path.has_value()) {
            return false;
        }

        auto lowered = *module_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.find(L"prospi-win64-shipping") != std::wstring::npos;
    }();

    return is_prospi;
}

bool nearly_equal(float value, float target, float tolerance) {
    return std::abs(value - target) <= tolerance;
}

int quantize_camera_value(float value, float step) {
    return static_cast<int>(std::round(value / step) * step);
}

std::string build_camera_calibration_id(const glm::vec3& location, const glm::vec3& rotation) {
    return std::format(
        "L({},{},{})_R({},{},{})",
        quantize_camera_value(location.x, 500.0f),
        quantize_camera_value(location.y, 500.0f),
        quantize_camera_value(location.z, 500.0f),
        quantize_camera_value(rotation.x, 5.0f),
        quantize_camera_value(rotation.y, 5.0f),
        quantize_camera_value(rotation.z, 5.0f)
    );
}

std::string build_generic_camera_preset_id(const glm::vec3& location, const glm::vec3& rotation, float fov) {
    return std::format(
        "L({},{},{})_R({},{},{})_F({})",
        quantize_camera_value(location.x, 500.0f),
        quantize_camera_value(location.y, 500.0f),
        quantize_camera_value(location.z, 500.0f),
        quantize_camera_value(rotation.x, 5.0f),
        quantize_camera_value(rotation.y, 5.0f),
        quantize_camera_value(rotation.z, 5.0f),
        quantize_camera_value(fov, 2.0f)
    );
}

std::filesystem::path get_camera_calibration_path() {
    return Framework::get_persistent_dir("camera_calibration.json");
}

std::filesystem::path get_generic_camera_presets_path() {
    return Framework::get_persistent_dir("camera_presets.json");
}

float smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float lerp_float(float a, float b, float t) {
    return a + (b - a) * t;
}

std::optional<float> get_runtime_cvar_float(std::wstring_view name) {
    static constexpr std::wstring_view modules[]{
        L"Renderer",
        L"Engine",
        L"Core",
        L"SlateRHIRenderer",
        L"Slate"
    };

    for (const auto module : modules) {
        if (auto value = sdk::get_cvar_float(module, name); value.has_value()) {
            return value;
        }
    }

    return std::nullopt;
}

std::optional<int> get_runtime_cvar_int(std::wstring_view name) {
    static constexpr std::wstring_view modules[]{
        L"Renderer",
        L"Engine",
        L"Core",
        L"SlateRHIRenderer",
        L"Slate"
    };

    for (const auto module : modules) {
        if (auto value = sdk::get_cvar_int(module, name); value.has_value()) {
            return value;
        }
    }

    return std::nullopt;
}

bool set_runtime_cvar_float(std::wstring_view name, float value) {
    static constexpr std::wstring_view modules[]{
        L"Renderer",
        L"Engine",
        L"Core",
        L"SlateRHIRenderer",
        L"Slate"
    };

    for (const auto module : modules) {
        if (sdk::set_cvar_float(module, name, value)) {
            return true;
        }
    }

    return false;
}

bool set_runtime_cvar_int(std::wstring_view name, int value) {
    static constexpr std::wstring_view modules[]{
        L"Renderer",
        L"Engine",
        L"Core",
        L"SlateRHIRenderer",
        L"Slate"
    };

    for (const auto module : modules) {
        if (sdk::set_cvar_int(module, name, value)) {
            return true;
        }
    }

    return false;
}

ProSpiCameraPreset classify_prospi_camera_preset(const glm::vec3& location, const glm::vec3& rotation, float raw_fov = 0.0f) {
    if (nearly_equal(location.x, 0.0f, 1200.0f) &&
        nearly_equal(location.y, 700.0f, 1400.0f) &&
        nearly_equal(location.z, 50.0f, 350.0f) &&
        nearly_equal(rotation.y, -85.0f, 22.0f) &&
        raw_fov >= 25.0f) {
        return ProSpiCameraPreset::GameplayBehindPlate;
    }

    if (nearly_equal(location.x, 0.0f, 1200.0f) &&
        nearly_equal(location.y, -500.0f, 1000.0f) &&
        nearly_equal(location.z, 0.0f, 450.0f) &&
        nearly_equal(rotation.x, -5.0f, 10.0f) &&
        rotation.y >= 50.0f && rotation.y <= 100.0f &&
        raw_fov >= 45.0f) {
        return ProSpiCameraPreset::HomePlateWaistHigh;
    }

    if (nearly_equal(location.x, 0.0f, 1200.0f) &&
        nearly_equal(location.y, -500.0f, 1000.0f) &&
        nearly_equal(location.z, 0.0f, 450.0f) &&
        rotation.x >= -10.0f && rotation.x <= 15.0f &&
        rotation.y >= -120.0f && rotation.y <= -60.0f &&
        raw_fov >= 30.0f) {
        return ProSpiCameraPreset::HomePlateWaistHighReverse;
    }

    if (nearly_equal(location.x, 0.0f, 2200.0f) &&
        nearly_equal(location.y, -3000.0f, 2600.0f) &&
        nearly_equal(location.z, 950.0f, 700.0f) &&
        nearly_equal(rotation.y, -95.0f, 20.0f) &&
        raw_fov >= 35.0f && raw_fov <= 55.0f) {
        return ProSpiCameraPreset::BehindPlateWideTelephoto;
    }

    if (location.x >= -1500.0f && location.x <= 3500.0f &&
        location.y >= -2000.0f && location.y <= 2500.0f &&
        nearly_equal(location.z, 1000.0f, 650.0f) &&
        rotation.x >= -35.0f && rotation.x <= 5.0f &&
        rotation.y >= -120.0f && rotation.y <= -35.0f &&
        raw_fov >= 40.0f && raw_fov <= 60.0f) {
        return ProSpiCameraPreset::BehindPlateElevatedSweep;
    }

    if (std::abs(location.x) >= 100000.0f &&
        std::abs(location.y) >= 25000.0f &&
        location.z >= 45000.0f &&
        raw_fov >= 55.0f &&
        (nearly_equal(rotation.y, -162.0f, 18.0f) || nearly_equal(rotation.y, 168.0f, 18.0f))) {
        return ProSpiCameraPreset::OpeningAerialTelephoto;
    }

    const auto is_tv_broadcast_yaw =
        (rotation.y >= 15.0f && rotation.y <= 70.0f) ||
        (rotation.y >= -140.0f && rotation.y <= -90.0f);

    const auto is_tv_close_cluster =
        raw_fov >= 55.0f && raw_fov <= 80.0f &&
        std::abs(location.x) >= 5000.0f &&
        nearly_equal(location.y, -2500.0f, 2200.0f) &&
        nearly_equal(location.z, 1000.0f, 900.0f) &&
        rotation.x >= -20.0f && rotation.x <= 15.0f &&
        is_tv_broadcast_yaw;

    const auto is_tv_far_cluster =
        raw_fov >= 55.0f && raw_fov <= 80.0f &&
        std::abs(location.x) >= 5000.0f &&
        location.y <= -7000.0f &&
        nearly_equal(location.z, 900.0f, 1200.0f) &&
        rotation.x >= -20.0f && rotation.x <= 15.0f &&
        is_tv_broadcast_yaw;

    if (is_tv_close_cluster || is_tv_far_cluster) {
        return ProSpiCameraPreset::TVBroadcast;
    }

    if (nearly_equal(location.x, 300.0f, 1800.0f) &&
        nearly_equal(location.y, 2600.0f, 1800.0f) &&
        nearly_equal(location.z, 900.0f, 900.0f) &&
        nearly_equal(rotation.y, -90.0f, 25.0f)) {
        return ProSpiCameraPreset::PlateHighTelephoto;
    }

    if (nearly_equal(location.x, 50.0f, 1800.0f) &&
        nearly_equal(location.y, 4050.0f, 1800.0f) &&
        nearly_equal(location.z, 2100.0f, 1000.0f) &&
        nearly_equal(rotation.y, -90.0f, 15.0f)) {
        return ProSpiCameraPreset::HomePlateOverheadTelephoto;
    }

    if (std::abs(location.x) <= 2500.0f &&
        nearly_equal(location.y, -11000.0f, 2400.0f) &&
        nearly_equal(location.z, 900.0f, 1200.0f) &&
        nearly_equal(rotation.y, 83.0f, 15.0f)) {
        return ProSpiCameraPreset::CenterFieldTelephoto;
    }

    if (std::abs(location.x) <= 2500.0f &&
        nearly_equal(location.y, -11000.0f, 2400.0f) &&
        nearly_equal(location.z, 2000.0f, 1200.0f) &&
        nearly_equal(rotation.y, 90.0f, 15.0f)) {
        return ProSpiCameraPreset::CenterFieldHighTelephoto;
    }

    if (nearly_equal(location.x, -1500.0f, 1800.0f) &&
        nearly_equal(location.y, -12000.0f, 2200.0f) &&
        nearly_equal(location.z, 1000.0f, 900.0f) &&
        rotation.x >= -5.0f && rotation.x <= 20.0f &&
        ((rotation.y >= 25.0f && rotation.y <= 70.0f) ||
         (rotation.y >= 110.0f && rotation.y <= 160.0f)) &&
        raw_fov >= 20.0f && raw_fov <= 45.0f) {
        return ProSpiCameraPreset::OffsetCenterFieldTelephoto;
    }

    if (nearly_equal(std::abs(location.x), 6640.0f, 2600.0f) &&
        nearly_equal(location.y, -7400.0f, 2200.0f) &&
        nearly_equal(location.z, 1750.0f, 900.0f) &&
        (nearly_equal(rotation.y, 41.0f, 20.0f) || nearly_equal(rotation.y, 140.0f, 20.0f) || nearly_equal(rotation.y, -160.0f, 20.0f))) {
        return ProSpiCameraPreset::DeepOutfieldTelephoto;
    }

    if (nearly_equal(location.x, 0.0f, 1600.0f) &&
        nearly_equal(location.y, -3500.0f, 1800.0f) &&
        nearly_equal(location.z, 5000.0f, 1800.0f) &&
        nearly_equal(rotation.x, -65.0f, 12.0f) &&
        (nearly_equal(rotation.y, 142.0f, 15.0f) || nearly_equal(rotation.y, -142.0f, 15.0f))) {
        return ProSpiCameraPreset::HomePlateSkyAerial;
    }

    if (nearly_equal(std::abs(location.x), 3400.0f, 1800.0f) &&
        nearly_equal(location.y, 2800.0f, 2200.0f) &&
        nearly_equal(location.z, 1350.0f, 800.0f) &&
        (nearly_equal(rotation.y, -56.0f, 22.0f) ||
         nearly_equal(rotation.y, -75.0f, 18.0f))) {
        return ProSpiCameraPreset::UpperDeckTelephoto;
    }

    if (nearly_equal(location.x, 3400.0f, 1800.0f) &&
        nearly_equal(location.y, 2800.0f, 2200.0f) &&
        nearly_equal(location.z, 1350.0f, 800.0f) &&
        (nearly_equal(rotation.y, -125.0f, 22.0f) ||
         nearly_equal(rotation.y, -135.0f, 18.0f))) {
        return ProSpiCameraPreset::UpperDeckHomeSkyTelephoto;
    }

    if (nearly_equal(location.x, -3230.0f, 2200.0f) &&
        nearly_equal(location.y, -540.0f, 1600.0f) &&
        nearly_equal(location.z, 30.0f, 500.0f) &&
        (nearly_equal(rotation.y, 0.0f, 16.0f) ||
         nearly_equal(rotation.y, -20.0f, 18.0f) ||
         nearly_equal(rotation.y, -45.0f, 18.0f))) {
        return ProSpiCameraPreset::ThirdBaseTelephoto;
    }

    if (nearly_equal(location.x, -3670.0f, 1800.0f) &&
        nearly_equal(location.y, -1010.0f, 1400.0f) &&
        nearly_equal(location.z, 30.0f, 500.0f) &&
        rotation.y >= -90.0f && rotation.y <= -55.0f &&
        raw_fov <= 24.0f) {
        return ProSpiCameraPreset::ThirdBaseRelayLow;
    }

    if (nearly_equal(location.x, -4000.0f, 1600.0f) &&
        nearly_equal(location.y, -6000.0f, 2200.0f) &&
        nearly_equal(location.z, 0.0f, 550.0f) &&
        rotation.x >= -10.0f && rotation.x <= 15.0f &&
        rotation.y >= 35.0f && rotation.y <= 80.0f &&
        raw_fov <= 22.0f) {
        return ProSpiCameraPreset::ThirdBaseCornerLow;
    }

    if (nearly_equal(location.x, -3230.0f, 2200.0f) &&
        nearly_equal(location.y, -540.0f, 1600.0f) &&
        nearly_equal(location.z, 30.0f, 500.0f) &&
        (nearly_equal(rotation.y, 25.0f, 18.0f) ||
         nearly_equal(rotation.y, 30.0f, 18.0f) ||
         nearly_equal(rotation.y, 45.0f, 18.0f))) {
        return ProSpiCameraPreset::ThirdBaseWideTelephoto;
    }

    if (nearly_equal(location.x, 3230.0f, 2200.0f) &&
        nearly_equal(location.y, -540.0f, 1600.0f) &&
        nearly_equal(location.z, 30.0f, 500.0f) &&
        (nearly_equal(rotation.y, -114.0f, 18.0f) ||
         nearly_equal(rotation.y, -100.0f, 18.0f) ||
         nearly_equal(rotation.y, -170.0f, 18.0f))) {
        return ProSpiCameraPreset::FirstBaseTelephoto;
    }

    if (nearly_equal(location.x, 3230.0f, 2200.0f) &&
        nearly_equal(location.y, -540.0f, 1600.0f) &&
        nearly_equal(location.z, 30.0f, 500.0f) &&
        (nearly_equal(rotation.y, -135.0f, 18.0f) ||
         nearly_equal(rotation.y, 170.0f, 18.0f) ||
         nearly_equal(rotation.y, 180.0f, 18.0f))) {
        return ProSpiCameraPreset::FirstBaseWideTelephoto;
    }

    if ((((nearly_equal(location.x, 2500.0f, 2200.0f) &&
           nearly_equal(location.y, 0.0f, 1200.0f)) ||
          (nearly_equal(location.x, 4000.0f, 1800.0f) &&
           nearly_equal(location.y, -6000.0f, 2200.0f))) &&
         nearly_equal(location.z, 0.0f, 550.0f) &&
         rotation.x >= -10.0f && rotation.x <= 15.0f &&
         rotation.y >= 105.0f && rotation.y <= 165.0f &&
         raw_fov <= 40.0f)) {
        return ProSpiCameraPreset::FirstBaseCornerLow;
    }

    if (location.x >= -500.0f && location.x <= 1200.0f &&
        location.y >= -800.0f && location.y <= 1200.0f &&
        nearly_equal(location.z, 0.0f, 450.0f) &&
        rotation.x >= -5.0f && rotation.x <= 20.0f &&
        rotation.y >= -180.0f && rotation.y <= -140.0f &&
        raw_fov >= 45.0f) {
        return ProSpiCameraPreset::FirstBaseInfieldLow;
    }

    if (location.x >= -200.0f && location.x <= 2200.0f &&
        location.y >= -100.0f && location.y <= 1700.0f &&
        nearly_equal(location.z, 0.0f, 350.0f) &&
        rotation.x >= 0.0f && rotation.x <= 20.0f &&
        rotation.y >= -155.0f && rotation.y <= -120.0f &&
        raw_fov >= 30.0f) {
        return ProSpiCameraPreset::LowInfieldSideCloseUp;
    }

    if (nearly_equal(location.x, 0.0f, 1200.0f) &&
        nearly_equal(location.y, 4000.0f, 1200.0f) &&
        nearly_equal(location.z, 2000.0f, 900.0f) &&
        nearly_equal(rotation.y, -110.0f, 12.0f) &&
        raw_fov <= 24.0f) {
        return ProSpiCameraPreset::BackstopHighTelephoto;
    }

    if (nearly_equal(location.x, 3000.0f, 1600.0f) &&
        nearly_equal(location.y, -500.0f, 1200.0f) &&
        nearly_equal(location.z, 0.0f, 700.0f) &&
        nearly_equal(rotation.y, 138.0f, 12.0f) &&
        raw_fov <= 24.0f) {
        return ProSpiCameraPreset::RightFieldCornerTelephoto;
    }

    if (nearly_equal(location.x, 4000.0f, 1800.0f) &&
        nearly_equal(location.y, -11500.0f, 1800.0f) &&
        nearly_equal(location.z, 1000.0f, 900.0f) &&
        nearly_equal(rotation.y, 110.0f, 12.0f) &&
        raw_fov <= 24.0f) {
        return ProSpiCameraPreset::RightCenterFieldTelephoto;
    }

    return ProSpiCameraPreset::None;
}

bool resolve_game_fov_offsets() {
    if (g_game_fov_resolver.attempted) {
        return g_game_fov_resolver.valid;
    }

    g_game_fov_resolver.attempted = true;

    auto pcm_class = sdk::APlayerCameraManager::static_class();
    if (pcm_class == nullptr) {
        return false;
    }

    auto find_cache_prop = [&](const wchar_t* name, int32_t& offset_out) -> sdk::FStructProperty* {
        auto prop = (sdk::FStructProperty*)pcm_class->find_property(name);
        if (prop != nullptr) {
            offset_out = prop->get_offset();
        }

        return prop;
    };

    auto cache_private_prop = find_cache_prop(L"CameraCachePrivate", g_game_fov_resolver.camera_cache_private_offset);
    auto cache_prop_public = find_cache_prop(L"CameraCache", g_game_fov_resolver.camera_cache_offset);
    auto last_frame_cache_private_prop = find_cache_prop(L"LastFrameCameraCachePrivate", g_game_fov_resolver.last_frame_camera_cache_private_offset);
    auto last_frame_cache_prop = find_cache_prop(L"LastFrameCameraCache", g_game_fov_resolver.last_frame_camera_cache_offset);

    sdk::FStructProperty* cache_prop = cache_private_prop;
    if (cache_prop == nullptr) {
        cache_prop = cache_prop_public;
    }

    if (cache_prop == nullptr) {
        cache_prop = last_frame_cache_private_prop;
    }

    if (cache_prop == nullptr) {
        cache_prop = last_frame_cache_prop;
    }

    if (cache_prop == nullptr) {
        return false;
    }

    g_game_fov_resolver.read_camera_cache_offset = cache_prop->get_offset();

    auto cache_struct = cache_prop->get_struct();
    if (cache_struct == nullptr) {
        return false;
    }

    auto pov_prop = (sdk::FStructProperty*)cache_struct->find_property(L"POV");
    if (pov_prop == nullptr) {
        return false;
    }

    auto pov_struct = pov_prop->get_struct();
    if (pov_struct == nullptr) {
        return false;
    }

    auto fov_prop = pov_struct->find_property(L"FOV");
    auto location_prop = pov_struct->find_property(L"Location");
    auto rotation_prop = pov_struct->find_property(L"Rotation");
    if (fov_prop == nullptr || location_prop == nullptr || rotation_prop == nullptr) {
        return false;
    }

    g_game_fov_resolver.pov_offset = pov_prop->get_offset();
    g_game_fov_resolver.fov_offset = fov_prop->get_offset();
    g_game_fov_resolver.location_offset = location_prop->get_offset();
    g_game_fov_resolver.rotation_offset = rotation_prop->get_offset();

    if (auto default_prop = pcm_class->find_property(L"DefaultFOV"); default_prop != nullptr) {
        g_game_fov_resolver.default_fov_offset = default_prop->get_offset();
    }

    g_game_fov_resolver.valid = true;
    return true;
}

std::optional<float> read_game_fov(sdk::APlayerCameraManager* pcm) {
    if (pcm == nullptr) {
        return std::nullopt;
    }

    if (!resolve_game_fov_offsets()) {
        return std::nullopt;
    }

    const auto base = (uint8_t*)pcm;
    const auto fov_ptr = (float*)(base + g_game_fov_resolver.read_camera_cache_offset +
                                  g_game_fov_resolver.pov_offset +
                                  g_game_fov_resolver.fov_offset);
    const auto fov = *fov_ptr;

    if (!std::isfinite(fov)) {
        return std::nullopt;
    }

    return fov;
}

std::optional<glm::vec3> read_game_camera_vector(sdk::APlayerCameraManager* pcm, int32_t field_offset) {
    if (pcm == nullptr) {
        return std::nullopt;
    }

    if (!resolve_game_fov_offsets() || field_offset < 0) {
        return std::nullopt;
    }

    const auto base = (uint8_t*)pcm;
    const auto vec_ptr = (float*)(base + g_game_fov_resolver.read_camera_cache_offset +
                                  g_game_fov_resolver.pov_offset +
                                  field_offset);

    const glm::vec3 value{vec_ptr[0], vec_ptr[1], vec_ptr[2]};
    if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) {
        return std::nullopt;
    }

    return value;
}

std::optional<glm::vec3> read_game_camera_location(sdk::APlayerCameraManager* pcm) {
    return read_game_camera_vector(pcm, g_game_fov_resolver.location_offset);
}

std::optional<glm::vec3> read_game_camera_rotation(sdk::APlayerCameraManager* pcm) {
    return read_game_camera_vector(pcm, g_game_fov_resolver.rotation_offset);
}

bool write_game_fov(sdk::APlayerCameraManager* pcm, float fov) {
    if (pcm == nullptr || !std::isfinite(fov)) {
        return false;
    }

    if (!resolve_game_fov_offsets()) {
        return false;
    }

    const auto base = (uint8_t*)pcm;
    const auto write_cache_fov = [&](int32_t cache_offset) {
        if (cache_offset < 0) {
            return;
        }

        auto fov_ptr = (float*)(base + cache_offset +
                                g_game_fov_resolver.pov_offset +
                                g_game_fov_resolver.fov_offset);
        *fov_ptr = fov;
    };

    write_cache_fov(g_game_fov_resolver.camera_cache_offset);
    write_cache_fov(g_game_fov_resolver.last_frame_camera_cache_offset);
    write_cache_fov(g_game_fov_resolver.camera_cache_private_offset);
    write_cache_fov(g_game_fov_resolver.last_frame_camera_cache_private_offset);

    return true;
}

std::optional<float> read_default_fov(sdk::APlayerCameraManager* pcm) {
    if (pcm == nullptr) {
        return std::nullopt;
    }

    if (!resolve_game_fov_offsets()) {
        return std::nullopt;
    }

    if (g_game_fov_resolver.default_fov_offset < 0) {
        return std::nullopt;
    }

    const auto base = (uint8_t*)pcm;
    const auto fov_ptr = (float*)(base + g_game_fov_resolver.default_fov_offset);
    const auto fov = *fov_ptr;

    if (!std::isfinite(fov)) {
        return std::nullopt;
    }

    return fov;
}

bool is_shf_executable() {
    static const bool is_shf = []() {
        const auto module_path = utility::get_module_pathw(utility::get_executable());
        if (!module_path.has_value()) {
            return false;
        }

        auto lowered = *module_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.find(L"shf-win64-shipping") != std::wstring::npos;
    }();

    return is_shf;
}

bool is_avowed_executable() {
    static const bool is_avowed = []() {
        const auto module_path = utility::get_module_pathw(utility::get_executable());
        if (!module_path.has_value()) {
            return false;
        }

        return uevr::games::is_avowed_executable_path(*module_path);
    }();

    return is_avowed;
}

bool is_dispatch_executable() {
    static const bool is_dispatch = []() {
        const auto module_path = utility::get_module_pathw(utility::get_executable());
        if (!module_path.has_value()) {
            return false;
        }

        auto lowered = *module_path;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });

        return lowered.find(L"dispatch-win64-shipping") != std::wstring::npos;
    }();

    return is_dispatch;
}

bool contains_case_insensitive(std::wstring_view value, std::wstring_view needle) {
    auto value_lower = std::wstring{value};
    auto needle_lower = std::wstring{needle};

    std::transform(value_lower.begin(), value_lower.end(), value_lower.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    std::transform(needle_lower.begin(), needle_lower.end(), needle_lower.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });

    return value_lower.find(needle_lower) != std::wstring::npos;
}

std::optional<std::wstring> read_object_text_property(sdk::UObject* object, std::wstring_view name) try {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return std::nullopt;
    }

    const auto klass = object->get_class();
    if (klass == nullptr) {
        return std::nullopt;
    }

    const auto prop = klass->find_property(name);
    if (prop == nullptr || prop->get_class() == nullptr) {
        return std::nullopt;
    }

    const auto prop_type = prop->get_class()->get_name().to_string();
    const auto prop_addr = (uint8_t*)object + prop->get_offset();

    if (prop_type == L"NameProperty") {
        return ((sdk::FName*)prop_addr)->to_string();
    }

    if (prop_type == L"StrProperty") {
        const auto str = (sdk::TArray<wchar_t>*)prop_addr;
        if (str->data == nullptr || str->count <= 0 || str->count > 4096 || str->capacity < str->count) {
            return std::wstring{};
        }

        if (IsBadReadPtr(str->data, (size_t)str->count * sizeof(wchar_t))) {
            return std::nullopt;
        }

        auto count = (size_t)str->count;
        while (count > 0 && str->data[count - 1] == L'\0') {
            --count;
        }

        return std::wstring{str->data, count};
    }

    return std::nullopt;
} catch (...) {
    return std::nullopt;
}

std::optional<sdk::UObject*> read_object_property(sdk::UObject* object, std::wstring_view name) try {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return std::nullopt;
    }

    const auto klass = object->get_class();
    if (klass == nullptr) {
        return std::nullopt;
    }

    const auto prop = klass->find_property(name);
    if (prop == nullptr || prop->get_class() == nullptr) {
        return std::nullopt;
    }

    const auto prop_type = prop->get_class()->get_name().to_string();
    if (prop_type != L"ObjectProperty") {
        return std::nullopt;
    }

    auto value = *(sdk::UObject**)((uint8_t*)object + prop->get_offset());
    if (value == nullptr || IsBadReadPtr(value, sizeof(void*))) {
        return std::nullopt;
    }

    return value;
} catch (...) {
    return std::nullopt;
}

std::optional<sdk::UObject*> read_struct_object_field(sdk::UObject* object, std::wstring_view struct_property, size_t field_offset) try {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return std::nullopt;
    }

    const auto klass = object->get_class();
    if (klass == nullptr) {
        return std::nullopt;
    }

    const auto prop = klass->find_property(struct_property);
    if (prop == nullptr || prop->get_class() == nullptr) {
        return std::nullopt;
    }

    const auto prop_type = prop->get_class()->get_name().to_string();
    if (prop_type != L"StructProperty") {
        return std::nullopt;
    }

    const auto value_addr = (uint8_t*)object + prop->get_offset() + field_offset;
    if (IsBadReadPtr(value_addr, sizeof(sdk::UObject*))) {
        return std::nullopt;
    }

    const auto value = *(sdk::UObject**)value_addr;
    if (value == nullptr || IsBadReadPtr(value, sizeof(void*))) {
        return std::nullopt;
    }

    return value;
} catch (...) {
    return std::nullopt;
}

std::optional<bool> call_object_bool_function(sdk::UObject* object, std::wstring_view function_name) try {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return std::nullopt;
    }

    const auto klass = object->get_class();
    if (klass == nullptr) {
        return std::nullopt;
    }

    const auto fn = klass->find_function(function_name);
    if (fn == nullptr) {
        return std::nullopt;
    }

    struct BoolReturnParams {
        bool ret{false};
    } params{};

    object->process_event(fn, &params);
    return params.ret;
} catch (...) {
    return std::nullopt;
}

std::string get_log_object_name(sdk::UObject* object) {
    if (object == nullptr || IsBadReadPtr(object, sizeof(void*))) {
        return "null";
    }

    try {
        return utility::narrow(object->get_full_name());
    } catch (...) {
        return "unresolved";
    }
}

std::vector<sdk::UObject*> get_live_objects_by_class_name(const std::wstring& class_name) {
    std::vector<sdk::UObject*> result{};

    const auto klass = sdk::find_uobject<sdk::UClass>(class_name);
    if (klass == nullptr) {
        return result;
    }

    auto& object_hook = UObjectHook::get();
    object_hook->activate();

    const auto cdo = klass->get_class_default_object();
    for (auto object_base : object_hook->get_objects_by_class(klass)) {
        if (object_base == nullptr || object_base == cdo) {
            continue;
        }

        auto object = (sdk::UObject*)object_base;
        if (object_hook->exists(object)) {
            result.push_back(object);
        }
    }

    return result;
}

sdk::UObject* get_first_live_object_by_class_name(const std::wstring& class_name) {
    const auto objects = get_live_objects_by_class_name(class_name);
    return objects.empty() ? nullptr : objects.front();
}

std::optional<sdk::UObject*> read_pcm_view_target(sdk::APlayerCameraManager* pcm) try {
    struct ViewTargetResolver {
        bool attempted{false};
        bool valid{false};
        int32_t view_target_offset{-1};
        int32_t target_offset{-1};
    };

    static ViewTargetResolver resolver{};

    if (pcm == nullptr) {
        return std::nullopt;
    }

    if (!resolver.attempted) {
        resolver.attempted = true;

        const auto pcm_class = sdk::APlayerCameraManager::static_class();
        if (pcm_class != nullptr) {
            if (const auto view_target_prop = (sdk::FStructProperty*)pcm_class->find_property(L"ViewTarget"); view_target_prop != nullptr) {
                if (const auto view_target_struct = view_target_prop->get_struct(); view_target_struct != nullptr) {
                    if (const auto target_prop = view_target_struct->find_property(L"Target"); target_prop != nullptr) {
                        resolver.view_target_offset = view_target_prop->get_offset();
                        resolver.target_offset = target_prop->get_offset();
                        resolver.valid = true;
                    }
                }
            }
        }
    }

    if (!resolver.valid) {
        return std::nullopt;
    }

    const auto target = *(sdk::UObject**)((uint8_t*)pcm + resolver.view_target_offset + resolver.target_offset);
    if (target == nullptr || IsBadReadPtr(target, sizeof(void*))) {
        return std::nullopt;
    }

    return target;
} catch (...) {
    return std::nullopt;
}

sdk::APlayerCameraManager* get_primary_player_camera_manager(sdk::UGameEngine* engine) {
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    auto gameplay = sdk::UGameplayStatics::get();

    if (world == nullptr || gameplay == nullptr) {
        return nullptr;
    }

    auto pc = gameplay->get_player_controller(world, 0);
    return pc != nullptr ? pc->get_player_camera_manager() : nullptr;
}

std::optional<std::wstring> find_shf_bink_url() {
    static const std::wstring tool_class_name = L"BlueprintGeneratedClass /Game/Cinematic/Asset/BinkPlayer/CS_BinkPlayTool.CS_BinkPlayTool_C";
    static const std::wstring player_class_name = L"Class /Script/BinkMediaPlayer.BinkMediaPlayer";

    constexpr std::wstring_view target_movie = L"noce_prerender_sc0101_l1_bk";

    for (auto* tool : get_live_objects_by_class_name(tool_class_name)) {
        for (const auto property_name : {L"CurrentBinkLinkUrl", L"BinkLinkUrl"}) {
            if (const auto url = read_object_text_property(tool, property_name); url.has_value() && contains_case_insensitive(*url, target_movie)) {
                return url;
            }
        }

        if (const auto player = read_object_property(tool, L"CurrentBinkPlayerSource"); player.has_value() && *player != nullptr) {
            if (const auto url = read_object_text_property(*player, L"URL"); url.has_value() && contains_case_insensitive(*url, target_movie)) {
                return url;
            }
        }
    }

    for (auto* player : get_live_objects_by_class_name(player_class_name)) {
        if (const auto url = read_object_text_property(player, L"URL"); url.has_value() && contains_case_insensitive(*url, target_movie)) {
            return url;
        }
    }

    return std::nullopt;
}

struct ShfAuto2DDecision {
    bool should_force{false};
    std::wstring cutscene{};
    std::wstring url{};
    std::wstring target{};
    std::optional<float> fov{};
};

ShfAuto2DDecision evaluate_shf_auto_2d(sdk::UGameEngine* engine) {
    static const std::wstring widget_class_name = L"WidgetBlueprintGeneratedClass /Game/UI/Cutscene/WBP_Cutscene.WBP_Cutscene_C";

    ShfAuto2DDecision decision{};

    sdk::UObject* widget = nullptr;
    for (auto* candidate : get_live_objects_by_class_name(widget_class_name)) {
        const auto candidate_cutscene = read_object_text_property(candidate, L"CutsceneName");
        if (candidate_cutscene.has_value() && *candidate_cutscene == L"LS_SC0101_L1_M") {
            widget = candidate;
            decision.cutscene = *candidate_cutscene;
            break;
        }
    }

    if (widget == nullptr) {
        return decision;
    }

    const auto bink_url = find_shf_bink_url();
    if (!bink_url.has_value()) {
        return decision;
    }

    decision.url = *bink_url;

    auto* pcm = get_primary_player_camera_manager(engine);
    decision.fov = read_game_fov(pcm);
    if (!decision.fov.has_value() || *decision.fov < 37.3f || *decision.fov > 37.7f) {
        return decision;
    }

    if (const auto target = read_pcm_view_target(pcm); target.has_value() && *target != nullptr) {
        decision.target = (*target)->get_full_name();
        if (!contains_case_insensitive(decision.target, L"CineCameraActor")) {
            return decision;
        }
    }

    decision.should_force = true;
    return decision;
}

struct DispatchAuto2DDecision {
    bool should_force{false};
    std::string reason{};
    std::string subsystem{};
    std::string source{};
    std::string player{};
    std::string texture{};
    std::optional<bool> playing{};
    std::optional<bool> preparing{};
    std::optional<bool> buffering{};
    std::optional<bool> ready{};
};

bool is_dispatch_media_player_active(sdk::UObject* player, DispatchAuto2DDecision& decision) {
    decision.playing = call_object_bool_function(player, L"IsPlaying");
    decision.preparing = call_object_bool_function(player, L"IsPreparing");
    decision.buffering = call_object_bool_function(player, L"IsBuffering");
    decision.ready = call_object_bool_function(player, L"IsReady");

    if (decision.playing.value_or(false)) {
        decision.reason = "media-player-playing";
        return true;
    }

    if (decision.preparing.value_or(false) || decision.buffering.value_or(false)) {
        decision.reason = "media-player-loading";
        return true;
    }

    // If UE4 media functions cannot be resolved in this title, trust Dispatch's
    // explicit ActiveMediaObjects struct instead of leaving movie scenes in HMD space.
    if (!decision.playing.has_value() && !decision.preparing.has_value() && !decision.buffering.has_value() && !decision.ready.has_value()) {
        decision.reason = "active-media-objects";
        return true;
    }

    return false;
}

DispatchAuto2DDecision evaluate_dispatch_auto_2d(sdk::UGameEngine* engine) {
    (void)engine;

    static const std::wstring media_subsystem_class_name = L"Class /Script/AdHocMedia.AdHocMediaSubsystem";
    static const std::wstring map_transition_widget_class_name =
        L"WidgetBlueprintGeneratedClass /Game/Shared/Shifts/Gameplay/Widgets/HeroDatabase/SubWidgets/WBP_VideoPlayerMapTransition.WBP_VideoPlayerMapTransition_C";

    DispatchAuto2DDecision decision{};

    for (auto* subsystem : get_live_objects_by_class_name(media_subsystem_class_name)) {
        const auto source = read_struct_object_field(subsystem, L"ActiveMediaObjects", 0x0);
        const auto player = read_struct_object_field(subsystem, L"ActiveMediaObjects", 0x8);
        const auto texture = read_struct_object_field(subsystem, L"ActiveMediaObjects", 0x10);

        if (!source.has_value() && !player.has_value() && !texture.has_value()) {
            continue;
        }

        decision.subsystem = get_log_object_name(subsystem);
        decision.source = source.has_value() ? get_log_object_name(*source) : "null";
        decision.player = player.has_value() ? get_log_object_name(*player) : "null";
        decision.texture = texture.has_value() ? get_log_object_name(*texture) : "null";

        if (player.has_value() && *player != nullptr && is_dispatch_media_player_active(*player, decision)) {
            decision.should_force = true;
            return decision;
        }
    }

    for (auto* widget : get_live_objects_by_class_name(map_transition_widget_class_name)) {
        const auto in_viewport = call_object_bool_function(widget, L"IsInViewport");
        const auto visible = call_object_bool_function(widget, L"IsVisible");
        if (in_viewport.value_or(false) || (!in_viewport.has_value() && visible.value_or(false))) {
            decision.should_force = true;
            decision.reason = "video-map-transition-widget";
            decision.player = get_log_object_name(widget);
            return decision;
        }
    }

    return decision;
}
}

bool VR::should_ignore_native_stereo_fix_for_avowed_sync() const {
    if (!is_avowed_executable() || !m_native_stereo_fix->value()) {
        return false;
    }

    if (m_rendering_method->value() != RenderingMethod::SYNCHRONIZED) {
        return false;
    }

    SPDLOG_INFO_ONCE("[Avowed][NativeStereoFix] Ignoring Native Stereo Fix while Synced Sequential rendering is active");
    return true;
}

bool VR::should_force_native_stereo_fix_same_pass() const {
    if (!m_native_stereo_fix->value() || is_using_afr() || !is_stalker2_executable_cached()) {
        return false;
    }

    // Stalker2's UE5.1 render-target handoff is only stable with the native
    // stereo fix using the original same-pass path. Letting this flip live can
    // invalidate active render state and crash during cutscene/gameplay RT work.
    SPDLOG_INFO_ONCE("[Stalker2][NativeStereoFix] Forcing Same Stereo Pass while Native Stereo Fix is enabled");
    return true;
}

bool VR::is_controller_camera_conflict_guard_active() const {
    if (!is_controller_camera_conflict_guard_enabled() || !is_hmd_active()) {
        return false;
    }

    static const bool is_supported_title = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_controller_camera_guard_candidate_path(*exe_path);
    }();

    if (!is_supported_title || !is_xinput_gamepad_active_within(std::chrono::seconds(2))) {
        return false;
    }

    // Keep the guard out of menus/loading screens. It is intended for gameplay
    // controller/camera conflicts after a pawn is actually possessed.
    try {
        const auto engine = sdk::UGameEngine::get();
        const auto world = engine != nullptr ? engine->get_world() : nullptr;
        const auto player_controller = world != nullptr && sdk::UGameplayStatics::get() != nullptr
            ? sdk::UGameplayStatics::get()->get_player_controller(world, 0)
            : nullptr;

        if (player_controller == nullptr || player_controller->get_acknowledged_pawn() == nullptr) {
            return false;
        }
    } catch (...) {
        return false;
    }

    SPDLOG_INFO_ONCE("[ControllerCameraGuard] Active: preserving game camera/control rotation while gamepad is active");
    return true;
}

// Called when the mod is initialized
std::optional<std::string> VR::clean_initialize() try {
    ZoneScopedN(__FUNCTION__);

    auto openvr_error = initialize_openvr();

    if (openvr_error || !m_openvr->loaded) {
        if (m_openvr->error) {
            spdlog::info("OpenVR failed to load: {}", *m_openvr->error);
        }

        m_openvr->is_hmd_active = false;
        m_openvr->was_hmd_active = false;
        m_openvr->needs_pose_update = false;

        // Attempt to load OpenXR instead
        auto openxr_error = initialize_openxr();

        if (openxr_error || !m_openxr->loaded) {
            m_openxr->needs_pose_update = false;
        }
    } else {
        m_openxr->error = "OpenVR loaded first.";
    }

    if (!get_runtime()->loaded) {
        // this is okay. we're not going to fail the whole thing entirely
        // so we're just going to return OK, but
        // when the VR mod draws its menu, it'll say "VR is not available"
        return Mod::on_initialize();
    }

    // Check whether the user has Hardware accelerated GPU scheduling enabled
    const auto hw_schedule_value = utility::get_registry_dword(
        HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers",
        "HwSchMode");

    if (hw_schedule_value) {
        m_has_hw_scheduling = *hw_schedule_value == 2;
    }

    m_init_finished = true;

    // all OK
    return Mod::on_initialize();
} catch(...) {
    spdlog::error("Exception occurred in VR::on_initialize()");

    m_runtime->error = "Exception occurred in VR::on_initialize()";
    m_openxr->dll_missing = false;
    m_openvr->dll_missing = false;
    m_openxr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->error = "Exception occurred in VR::on_initialize()";
    m_openvr->loaded = false;
    m_openvr->is_hmd_active = false;
    m_openxr->loaded = false;
    m_init_finished = false;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr() {
    ZoneScopedN(__FUNCTION__);

    spdlog::info("Attempting to load OpenVR");

    m_openvr = std::make_shared<runtimes::OpenVR>();
    m_openvr->loaded = false;

    const auto wants_openxr = m_requested_runtime_name->value() == "openxr_loader.dll";

    SPDLOG_INFO("[VR] Requested runtime: {}", m_requested_runtime_name->value());

    if (wants_openxr && GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
        // pre-injected
        m_openvr->dll_missing = true;
        m_openvr->error = "OpenXR already loaded";
        return Mod::on_initialize();
    }

    if (GetModuleHandleW(L"openvr_api.dll") == nullptr) {
        // pre-injected
        if (GetModuleHandleW(L"openxr_loader.dll") != nullptr) {
            m_openvr->dll_missing = true;
            m_openvr->error = "OpenXR already loaded";
            return Mod::on_initialize();
        }


        if (utility::load_module_from_current_directory(L"openvr_api.dll") == nullptr) {
            spdlog::info("[VR] Could not load openvr_api.dll");

            m_openvr->dll_missing = true;
            m_openvr->error = "Could not load openvr_api.dll";
            return Mod::on_initialize();
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openvr->needs_pose_update = true;
    m_openvr->got_first_poses = false;
    m_openvr->is_hmd_active = true;
    m_openvr->was_hmd_active = true;

    spdlog::info("Attempting to call vr::VR_Init");

    auto error = vr::VRInitError_None;
	m_openvr->hmd = vr::VR_Init(&error, vr::VRApplication_Scene);

    // check if error
    if (error != vr::VRInitError_None) {
        m_openvr->error = "VR_Init failed: " + std::string{vr::VR_GetVRInitErrorAsEnglishDescription(error)};
        return Mod::on_initialize();
    }

    if (m_openvr->hmd == nullptr) {
        m_openvr->error = "VR_Init failed: HMD is null";
        return Mod::on_initialize();
    }

    // get render target size
    m_openvr->update_render_target_size();

    if (vr::VRCompositor() == nullptr) {
        m_openvr->error = "VRCompositor failed to initialize.";
        return Mod::on_initialize();
    }

    auto input_error = initialize_openvr_input();

    if (input_error) {
        m_openvr->error = *input_error;
        return Mod::on_initialize();
    }

    auto overlay_error = m_overlay_component.on_initialize_openvr();

    if (overlay_error) {
        m_openvr->error = *overlay_error;
        return Mod::on_initialize();
    }
    
    m_openvr->loaded = true;
    m_openvr->error = std::nullopt;
    m_runtime = m_openvr;

    return Mod::on_initialize();
}

std::optional<std::string> VR::initialize_openvr_input() {
    ZoneScopedN(__FUNCTION__);

    const auto module_directory = Framework::get_persistent_dir();

    // write default actions and bindings with the static strings we have
    for (auto& it : m_binding_files) {
        spdlog::info("Writing default binding file {}", it.first);

        std::ofstream file{ module_directory / it.first };
        file << it.second;
    }

    const auto actions_path = module_directory / "actions.json";
    auto input_error = vr::VRInput()->SetActionManifestPath(actions_path.string().c_str());

    if (input_error != vr::VRInputError_None) {
        return "VRInput failed to set action manifest path: " + std::to_string((uint32_t)input_error);
    }

    // get action set
    auto action_set_error = vr::VRInput()->GetActionSetHandle("/actions/default", &m_action_set);

    if (action_set_error != vr::VRInputError_None) {
        return "VRInput failed to get action set: " + std::to_string((uint32_t)action_set_error);
    }

    if (m_action_set == vr::k_ulInvalidActionSetHandle) {
        return "VRInput failed to get action set handle.";
    }

    for (auto& it : m_action_handles) {
        auto error = vr::VRInput()->GetActionHandle(it.first.c_str(), &it.second.get());

        if (error != vr::VRInputError_None) {
            return "VRInput failed to get action handle: (" + it.first + "): " + std::to_string((uint32_t)error);
        }

        if (it.second == vr::k_ulInvalidActionHandle) {
            return "VRInput failed to get action handle: (" + it.first + ")";
        }
    }

    m_active_action_set.ulActionSet = m_action_set;
    m_active_action_set.ulRestrictedToDevice = vr::k_ulInvalidInputValueHandle;
    m_active_action_set.nPriority = 0;

    m_openvr->pose_action = m_action_pose;
    m_openvr->grip_pose_action = m_action_grip_pose;

    detect_controllers();

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr() {
    ZoneScopedN(__FUNCTION__);

    m_openxr.reset();
    m_openxr = std::make_shared<runtimes::OpenXR>();

    spdlog::info("[VR] Initializing OpenXR");

    if (GetModuleHandleW(L"openxr_loader.dll") == nullptr) {
        if (utility::load_module_from_current_directory(L"openxr_loader.dll") == nullptr) {
            spdlog::info("[VR] Could not load openxr_loader.dll");

            m_openxr->loaded = false;
            m_openxr->error = "Could not load openxr_loader.dll";

            return std::nullopt;
        }
    }

    if (g_framework->is_dx12()) {
        m_d3d12.on_reset(this);
    } else {
        m_d3d11.on_reset(this);
    }

    m_openxr->needs_pose_update = true;
    m_openxr->got_first_poses = false;

    // Step 1: Create an instance
    spdlog::info("[VR] Creating OpenXR instance");

    XrResult result{XR_SUCCESS};

    // We may just be restarting OpenXR, so try to find an existing instance first
    if (m_openxr->instance == XR_NULL_HANDLE) {
        std::vector<const char*> extensions{};

        if (g_framework->is_dx12()) {
            extensions.push_back(XR_KHR_D3D12_ENABLE_EXTENSION_NAME);
        } else {
            extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        }

        // Enumerate available extensions and enable depth extension if available
        uint32_t extension_count{};
        result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &extension_count, nullptr);

        std::vector<XrExtensionProperties> extension_properties(extension_count, {XR_TYPE_EXTENSION_PROPERTIES});

        if (!XR_FAILED(result)) try {
            result = xrEnumerateInstanceExtensionProperties(nullptr, extension_count, &extension_count, extension_properties.data());

            if (!XR_FAILED(result)) {
                for (const auto& extension_property : extension_properties) {
                    spdlog::info("[VR] Found OpenXR extension: {}", extension_property.extensionName);
                }

                const std::unordered_set<std::string> wanted_extensions {
                    XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME,
                    XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME
                    // To be seen if we need more!
                };

                for (const auto& extension_property : extension_properties) {
                    if (wanted_extensions.contains(extension_property.extensionName)) {
                        spdlog::info("[VR] Enabling {} extension", extension_property.extensionName);
                        m_openxr->enabled_extensions.insert(extension_property.extensionName);
                        extensions.push_back(extension_property.extensionName);
                    }
                }
            }
        } catch(...) {
            spdlog::error("[VR] Unknown error while enumerating OpenXR extensions");
        }

        XrInstanceCreateInfo instance_create_info{XR_TYPE_INSTANCE_CREATE_INFO};
        instance_create_info.next = nullptr;
        instance_create_info.enabledExtensionCount = (uint32_t)extensions.size();
        instance_create_info.enabledExtensionNames = extensions.data();

        std::string application_name{"UEVR"};

        // Append the current executable name to the application base name
        {
            const auto exe = utility::get_executable();
            const auto full_path = utility::get_module_pathw(exe);

            if (full_path) {
                const auto fs_path = std::filesystem::path(*full_path);
                const auto filename = fs_path.stem().string();

                application_name += "_" + filename;

                // Trim the name to 127 characters
                if (application_name.length() >= XR_MAX_APPLICATION_NAME_SIZE) {
                    application_name = application_name.substr(0, XR_MAX_APPLICATION_NAME_SIZE - 1);
                }
            }
        }

        spdlog::info("[VR] Application name: {}", application_name);

        strcpy(instance_create_info.applicationInfo.applicationName, application_name.c_str());
        instance_create_info.applicationInfo.applicationName[XR_MAX_APPLICATION_NAME_SIZE - 1] = '\0';
        instance_create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
        
        result = xrCreateInstance(&instance_create_info, &m_openxr->instance);

        // we can't convert the result to a string here
        // because the function requires the instance to be valid
        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr instance: " + std::to_string((int32_t)result);
            if (result == XR_ERROR_LIMIT_REACHED) {
                m_openxr->error = "Could not create openxr instance: XR_ERROR_LIMIT_REACHED\n"
                    "Ensure that the OpenXR plugin has been renamed or deleted from the game's binaries folder.";
            }
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr instance");
    }
    
    // Step 2: Create a system
    spdlog::info("[VR] Creating OpenXR system");

    // We may just be restarting OpenXR, so try to find an existing system first
    if (m_openxr->system == XR_NULL_SYSTEM_ID) {
        XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
        system_info.formFactor = m_openxr->form_factor;

        result = xrGetSystem(m_openxr->instance, &system_info, &m_openxr->system);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr system: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    } else {
        spdlog::info("[VR] Found existing openxr system");
    }

    // Step 3: Create a session
    spdlog::info("[VR] Initializing graphics info");

    XrSessionCreateInfo session_create_info{XR_TYPE_SESSION_CREATE_INFO};

    if (g_framework->is_dx12()) {
        m_d3d12.openxr().initialize(session_create_info);
    } else {
        m_d3d11.openxr().initialize(session_create_info);
    }

    spdlog::info("[VR] Creating OpenXR session");
    session_create_info.systemId = m_openxr->system;
    result = xrCreateSession(m_openxr->instance, &session_create_info, &m_openxr->session);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not create openxr session: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    // Step 4: Create a space
    spdlog::info("[VR] Creating OpenXR space");

    // We may just be restarting OpenXR, so try to find an existing space first

    if (m_openxr->stage_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->stage_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr stage space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    if (m_openxr->view_space == XR_NULL_HANDLE) {
        XrReferenceSpaceCreateInfo space_create_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space_create_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        space_create_info.poseInReferenceSpace = {};
        space_create_info.poseInReferenceSpace.orientation.w = 1.0f;

        result = xrCreateReferenceSpace(m_openxr->session, &space_create_info, &m_openxr->view_space);

        if (result != XR_SUCCESS) {
            m_openxr->error = "Could not create openxr view space: " + m_openxr->get_result_string(result);
            spdlog::error("[VR] {}", m_openxr->error.value());

            return std::nullopt;
        }
    }

    // Step 5: Get the system properties
    spdlog::info("[VR] Getting OpenXR system properties");

    XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
    result = xrGetSystemProperties(m_openxr->instance, m_openxr->system, &system_properties);

    if (result != XR_SUCCESS) {
        m_openxr->error = "Could not get system properties: " + m_openxr->get_result_string(result);
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->on_system_properties_acquired(system_properties);

    // Step 6: Get the view configuration properties
    m_openxr->update_render_target_size();

    // Step 7: Create a view
    if (!m_openxr->view_configs.empty()){
        m_openxr->views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
        m_openxr->stage_views.resize(m_openxr->view_configs.size(), {XR_TYPE_VIEW});
    }

    if (m_openxr->view_configs.empty()) {
        m_openxr->error = "No view configurations found";
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    m_openxr->loaded = true;
    m_runtime = m_openxr;

    if (auto err = initialize_openxr_input()) {
        m_openxr->error = err.value();
        m_openxr->loaded = false;
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }

    detect_controllers();

    if (m_init_finished) {
        // This is usually done in on_config_load
        // but the runtime can be reinitialized, so we do it here instead
        initialize_openxr_swapchains();
    }

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_input() {
    ZoneScopedN(__FUNCTION__);

    if (auto err = m_openxr->initialize_actions(VR::actions_json)) {
        m_openxr->error = err.value();
        spdlog::error("[VR] {}", m_openxr->error.value());

        return std::nullopt;
    }
    
    for (auto& it : m_action_handles) {
        auto openxr_action_name = m_openxr->translate_openvr_action_name(it.first);

        if (m_openxr->action_set.action_map.contains(openxr_action_name)) {
            it.second.get() = (decltype(it.second)::type)m_openxr->action_set.action_map[openxr_action_name];
            spdlog::info("[VR] Successfully mapped action {} to {}", it.first, openxr_action_name);
        }
    }

    m_left_joystick = (decltype(m_left_joystick))VRRuntime::Hand::LEFT;
    m_right_joystick = (decltype(m_right_joystick))VRRuntime::Hand::RIGHT;

    return std::nullopt;
}

std::optional<std::string> VR::initialize_openxr_swapchains() {
    ZoneScopedN(__FUNCTION__);

    // This depends on the config being loaded.
    if (!m_init_finished) {
        return std::nullopt;
    }

    spdlog::info("[VR] Creating OpenXR swapchain");

    const auto supported_swapchain_formats = m_openxr->get_supported_swapchain_formats();

    // Log
    for (auto f : supported_swapchain_formats) {
        spdlog::info("[VR] Supported swapchain format: {}", (uint32_t)f);
    }

    if (g_framework->is_dx12()) {
        auto err = m_d3d12.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());

            return m_openxr->error;
        }
    } else {
        auto err = m_d3d11.openxr().create_swapchains();

        if (err) {
            m_openxr->error = err.value();
            m_openxr->loaded = false;
            spdlog::error("[VR] {}", m_openxr->error.value());
            return m_openxr->error;
        }
    }

    return std::nullopt;
}

bool VR::detect_controllers() {
    ZoneScopedN(__FUNCTION__);

    // already detected
    if (!m_controllers.empty()) {
        return true;
    }

    if (get_runtime()->is_openvr()) {
        auto left_joystick_origin_error = vr::EVRInputError::VRInputError_None;
        auto right_joystick_origin_error = vr::EVRInputError::VRInputError_None;

        vr::InputOriginInfo_t left_joystick_origin_info{};
        vr::InputOriginInfo_t right_joystick_origin_info{};

        // Get input origin info for the joysticks
        // get the source input device handles for the joysticks
        auto left_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/left", &m_left_joystick);

        if (left_joystick_error != vr::VRInputError_None) {
            return false;
        }

        auto right_joystick_error = vr::VRInput()->GetInputSourceHandle("/user/hand/right", &m_right_joystick);

        if (right_joystick_error != vr::VRInputError_None) {
            return false;
        }

        m_openvr->left_controller_handle = m_left_joystick;
        m_openvr->right_controller_handle = m_right_joystick;

        left_joystick_origin_info = {};
        right_joystick_origin_info = {};

        left_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_left_joystick, &left_joystick_origin_info, sizeof(left_joystick_origin_info));
        right_joystick_origin_error = vr::VRInput()->GetOriginTrackedDeviceInfo(m_right_joystick, &right_joystick_origin_info, sizeof(right_joystick_origin_info));
        if (left_joystick_origin_error != vr::EVRInputError::VRInputError_None || right_joystick_origin_error != vr::EVRInputError::VRInputError_None) {
            return false;
        }

        // Instead of manually going through the devices,
        // We do this. The order of the devices isn't always guaranteed to be
        // Left, and then right. Using the input state handles will always
        // Get us the correct device indices.
        m_controllers.push_back(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers.push_back(right_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(left_joystick_origin_info.trackedDeviceIndex);
        m_controllers_set.insert(right_joystick_origin_info.trackedDeviceIndex);

        spdlog::info("Left Hand: {}", left_joystick_origin_info.trackedDeviceIndex);
        spdlog::info("Right Hand: {}", right_joystick_origin_info.trackedDeviceIndex);

        m_openvr->left_controller_index = left_joystick_origin_info.trackedDeviceIndex;
        m_openvr->right_controller_index = right_joystick_origin_info.trackedDeviceIndex;
    } else if (get_runtime()->is_openxr()) {
        // ezpz
        m_controllers.push_back(1);
        m_controllers.push_back(2);
        m_controllers_set.insert(1);
        m_controllers_set.insert(2);

        spdlog::info("Left Hand: {}", 1);
        spdlog::info("Right Hand: {}", 2);
    }


    return true;
}

bool VR::is_any_action_down() {
    ZoneScopedN(__FUNCTION__);

    if (!m_runtime->ready()) {
        return false;
    }

    const auto left_axis = get_left_stick_axis();
    const auto right_axis = get_right_stick_axis();

    if (glm::length(left_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    if (glm::length(right_axis) >= m_joystick_deadzone->value()) {
        return true;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();

    for (auto& it : m_action_handles) {
        // These are too easy to trigger
        if (it.second == m_action_thumbrest_touch_left || it.second == m_action_thumbrest_touch_right) {
            continue;
        }

        if (it.second == m_action_a_button_touch_left || it.second == m_action_a_button_touch_right) {
            continue;
        }

        if (it.second == m_action_b_button_touch_left || it.second == m_action_b_button_touch_right) {
            continue;
        }

        if (is_action_active(it.second, left_joystick) || is_action_active(it.second, right_joystick)) {
            return true;
        }
    }

    return false;
}

bool VR::on_message(HWND wnd, UINT message, WPARAM w_param, LPARAM l_param) {
    ZoneScopedN(__FUNCTION__);

    if (message == WM_DEVICECHANGE && !m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Received WM_DEVICECHANGE");
        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    return true;
}

void VR::on_xinput_get_state(uint32_t* retval, uint32_t user_index, XINPUT_STATE* state) {
    ZoneScopedN(__FUNCTION__);

    const auto now = std::chrono::steady_clock::now();

    if (now - m_last_engine_tick > std::chrono::seconds(1)) {
        const auto mod_frame_delta_ms = m_last_mod_frame.time_since_epoch().count() == 0
            ? -1ll
            : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_mod_frame).count();
        const auto tick_delta_ms = m_last_engine_tick.time_since_epoch().count() == 0
            ? -1ll
            : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_engine_tick).count();

        if (const auto runtime = get_runtime(); runtime != nullptr && runtime->is_openxr()) {
            if (const auto openxr = get_openxr_runtime(); openxr != nullptr) {
                SPDLOG_INFO_EVERY_N_SEC(
                    1,
                    "[VR] XInputGetState called, but engine tick hasn't been called in over a second. tick_delta_ms={} mod_frame_delta_ms={} session_state={} session_ready={} frame_synced={} frame_began={} got_first_poses={} got_first_valid_poses={}",
                    tick_delta_ms,
                    mod_frame_delta_ms,
                    openxr->get_session_state_string(openxr->session_state),
                    openxr->session_ready,
                    openxr->frame_synced,
                    openxr->frame_began,
                    openxr->got_first_poses,
                    openxr->got_first_valid_poses
                );
            } else {
                SPDLOG_INFO_EVERY_N_SEC(1, "[VR] XInputGetState called, but engine tick hasn't been called in over a second. tick_delta_ms={} mod_frame_delta_ms={}", tick_delta_ms, mod_frame_delta_ms);
            }
        } else {
            SPDLOG_INFO_EVERY_N_SEC(1, "[VR] XInputGetState called, but engine tick hasn't been called in over a second. tick_delta_ms={} mod_frame_delta_ms={}", tick_delta_ms, mod_frame_delta_ms);
        }

        update_action_states();
    }

    if (*retval == ERROR_SUCCESS) {
        // Once here for normal gamepads, and once for the spoofed gamepad at the end
        update_imgui_state_from_xinput_state(*state, false);
        gamepad_snapturn(*state);
    }

    if (now - m_last_xinput_update > std::chrono::seconds(2)) {
        m_lowest_xinput_user_index = user_index;
    }

    if (user_index < m_lowest_xinput_user_index) {
        m_lowest_xinput_user_index = user_index;
        spdlog::info("[VR] Changed lowest XInput user index to {}", user_index);
    }

    if (user_index != m_lowest_xinput_user_index) {
        if (!m_spoofed_gamepad_connection && is_using_controllers()) {
            spdlog::info("[VR] XInputGetState called, but user index is {}", user_index);
        }

        return;
    }

    if (!m_spoofed_gamepad_connection) {
        spdlog::info("[VR] Successfully spoofed gamepad connection @ {}", user_index);
    }
    
    m_last_xinput_update = now;
    m_spoofed_gamepad_connection = true;

    auto runtime = get_runtime();

    auto do_pause_select = [&]() {
        if (!runtime->ready()) {
            return;
        }

        if (runtime->handle_pause) {
            // Spoof the start button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
            *retval = ERROR_SUCCESS;
            runtime->handle_pause = false;
            runtime->handle_select_button = false;
        }

        if (runtime->handle_select_button) {
            // Spoof the back button being pressed
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
            *retval = ERROR_SUCCESS;
            runtime->handle_select_button = false;
            runtime->handle_pause = false;
        }
    };

    do_pause_select();

    if (is_using_controllers_within(std::chrono::minutes(5))) {
        *retval = ERROR_SUCCESS;
    }

    if (!is_using_controllers()) {
        return;
    }

    // Clear button state for VR controllers
    if (is_using_controllers_within(std::chrono::seconds(5))) {
        state->Gamepad.wButtons = 0;
        state->Gamepad.bLeftTrigger = 0;
        state->Gamepad.bRightTrigger = 0;
        state->Gamepad.sThumbLX = 0;
        state->Gamepad.sThumbLY = 0;
        state->Gamepad.sThumbRX = 0;
        state->Gamepad.sThumbRY = 0;
    }

    const auto left_joystick = get_left_joystick();
    const auto right_joystick = get_right_joystick();
    const auto wants_swap = m_swap_controllers->value();

    runtime->handle_pause_select(is_action_active_any_joystick(m_action_system_button));
    do_pause_select();

    const auto& a_button_left = !wants_swap ? m_action_a_button_left : m_action_a_button_right;
    const auto& a_button_right = !wants_swap ? m_action_a_button_right : m_action_a_button_left;

    const auto is_right_a_button_down = is_action_active_any_joystick(a_button_right);
    const auto is_left_a_button_down = is_action_active_any_joystick(a_button_left);

    if (is_right_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    }

    if (is_left_a_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    }

    const auto& b_button_left = !wants_swap ? m_action_b_button_left : m_action_b_button_right;
    const auto& b_button_right = !wants_swap ? m_action_b_button_right : m_action_b_button_left;

    const auto is_right_b_button_down = is_action_active_any_joystick(b_button_right);
    const auto is_left_b_button_down = is_action_active_any_joystick(b_button_left);

    if (is_right_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    }

    if (is_left_b_button_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    }

    const auto is_left_joystick_click_down = is_action_active(m_action_joystick_click, left_joystick);
    const auto is_right_joystick_click_down = is_action_active(m_action_joystick_click, right_joystick);

    if (is_left_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    }

    if (is_right_joystick_click_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    }

    const auto is_left_trigger_down = is_action_active(m_action_trigger, left_joystick);
    const auto is_right_trigger_down = is_action_active(m_action_trigger, right_joystick);

    if (is_left_trigger_down) {
        state->Gamepad.bLeftTrigger = 255;
    }

    if (is_right_trigger_down) {
        state->Gamepad.bRightTrigger = 255;
    }

    const auto is_right_grip_down = is_action_active(m_action_grip, right_joystick);
    const auto is_left_grip_down = is_action_active(m_action_grip, left_joystick);

    if (is_right_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }

    if (is_left_grip_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    }

    const auto is_dpad_up_down = is_action_active_any_joystick(m_action_dpad_up);

    if (is_dpad_up_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    }

    const auto is_dpad_right_down = is_action_active_any_joystick(m_action_dpad_right);

    if (is_dpad_right_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    }

    const auto is_dpad_down_down = is_action_active_any_joystick(m_action_dpad_down);

    if (is_dpad_down_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    }

    const auto is_dpad_left_down = is_action_active_any_joystick(m_action_dpad_left);

    if (is_dpad_left_down) {
        state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    }

    const auto left_joystick_axis = get_joystick_axis(left_joystick);
    const auto right_joystick_axis = get_joystick_axis(right_joystick);

    const auto true_left_joystick_axis = get_joystick_axis(m_left_joystick);
    const auto true_right_joystick_axis = get_joystick_axis(m_right_joystick);

    state->Gamepad.sThumbLX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLX + left_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbLY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbLY + left_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    state->Gamepad.sThumbRX = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRX + right_joystick_axis.x * 32767.0f), -32767.0f, 32767.0f);
    state->Gamepad.sThumbRY = (int16_t)std::clamp<float>(((float)state->Gamepad.sThumbRY + right_joystick_axis.y * 32767.0f), -32767.0f, 32767.0f);

    bool already_dpad_shifted{false};
    bool true_left_joystick_as_dpad{false}; 
    bool true_right_joystick_as_dpad{false}; 

    if (m_dpad_gesture_state.direction != DPadGestureState::Direction::NONE) {
        already_dpad_shifted = true;

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::UP) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::RIGHT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::DOWN) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        }

        if ((m_dpad_gesture_state.direction & DPadGestureState::Direction::LEFT) != 0) {
            state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        }

        DPadMethod dpad_method = get_dpad_method();
        if (dpad_method == DPadMethod::GESTURE_HEAD){
            true_left_joystick_as_dpad = true;
        }
        else if (dpad_method == DPadMethod::GESTURE_HEAD_RIGHT) {
            true_right_joystick_as_dpad = true;
        }


        std::scoped_lock _{m_dpad_gesture_state.mtx};
        m_dpad_gesture_state.direction = DPadGestureState::Direction::NONE;
    }

    // Touching the thumbrest allows us to use the thumbstick as a dpad.  Additional options are for controllers without capacitives/games that rely solely on DPad
    if (!already_dpad_shifted && m_dpad_shifting->value()) {
        bool button_touch_inactive{true};
        bool thumbrest_check{false};

        DPadMethod dpad_method = get_dpad_method();
        if (dpad_method == DPadMethod::RIGHT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_right);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_right) && !is_action_active_any_joystick(m_action_b_button_touch_right);
        }
        if (dpad_method == DPadMethod::LEFT_TOUCH) {
            thumbrest_check = is_action_active_any_joystick(m_action_thumbrest_touch_left);
            button_touch_inactive = !is_action_active_any_joystick(m_action_a_button_touch_left) && !is_action_active_any_joystick(m_action_b_button_touch_left);
        }

        // Toggling UEVR menu using L3 + R3 has higher priority 
        const auto dpad_active = (is_right_joystick_click_down &&  (dpad_method == DPadMethod::RIGHT_JOYSTICK_CLICK) && (! is_left_joystick_click_down)) 
        || (is_left_joystick_click_down &&  (dpad_method == DPadMethod::LEFT_JOYSTICK_CLICK) && (! is_right_joystick_click_down)) 
        || (button_touch_inactive && thumbrest_check) || dpad_method == DPadMethod::LEFT_JOYSTICK || dpad_method == DPadMethod::RIGHT_JOYSTICK;

        if (dpad_active) {
            float ty{0.0f};
            float tx{0.0f};
            //SHORT ThumbY{0};
            //SHORT ThumbX{0};
            // If someone is accidentally touching both thumbrests while also moving a joystick, this will default to left joystick.
            if (dpad_method == DPadMethod::RIGHT_TOUCH || dpad_method == DPadMethod::LEFT_JOYSTICK || dpad_method == DPadMethod::RIGHT_JOYSTICK_CLICK) {
                //ThumbY = state->Gamepad.sThumbLY;
                //ThumbX = state->Gamepad.sThumbLX;
                ty = true_left_joystick_axis.y;
                tx = true_left_joystick_axis.x;
                true_left_joystick_as_dpad = true;
            }
            else if (dpad_method == DPadMethod::LEFT_TOUCH || dpad_method == DPadMethod::RIGHT_JOYSTICK || dpad_method == DPadMethod::LEFT_JOYSTICK_CLICK) {
                //ThumbY = state->Gamepad.sThumbRY;
                //ThumbX = state->Gamepad.sThumbRX;
                ty = true_right_joystick_axis.y;
                tx = true_right_joystick_axis.x;
                true_right_joystick_as_dpad = true;
            }
            
            if (ty >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            }

            if (ty <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            }

            if (tx >= 0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            }

            if (tx <= -0.5f) {
                state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
            }

            if(dpad_method == DPadMethod::RIGHT_JOYSTICK_CLICK)
            {
                state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_RIGHT_THUMB;
            }
            else if(dpad_method == DPadMethod::LEFT_JOYSTICK_CLICK) 
            {
                state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_LEFT_THUMB;
            }

        }
    }

    // Zero out the thumbstick values
    if (true_left_joystick_as_dpad) {
        if (!wants_swap) {
            state->Gamepad.sThumbLY = 0;
            state->Gamepad.sThumbLX = 0;
        } else {
            state->Gamepad.sThumbRY = 0;
            state->Gamepad.sThumbRX = 0;
        }
    }
    else if (true_right_joystick_as_dpad) {
        if (!wants_swap) {
            state->Gamepad.sThumbRY = 0;
            state->Gamepad.sThumbRX = 0;
        } else {
            state->Gamepad.sThumbLY = 0;
            state->Gamepad.sThumbLX = 0;
        }
    }



    // Determine if snapturn should be run on frame
    if (m_snapturn->value()) {
        DPadMethod dpad_method = get_dpad_method();
        const auto snapturn_deadzone = get_snapturn_js_deadzone();
        float stick_axis{};

        if (!m_was_snapturn_run_on_input) {
            if (dpad_method == RIGHT_JOYSTICK) {
                stick_axis = true_left_joystick_axis.x;
                if (glm::abs(stick_axis) >= snapturn_deadzone) {
                    if (stick_axis < 0) {
                        m_snapturn_left = true;
                    }
                    m_snapturn_on_frame = true;
                    m_was_snapturn_run_on_input = true;
                }
            }
            else {
                stick_axis = right_joystick_axis.x;
                const auto& thumbrest_touch_left = !wants_swap ? m_action_thumbrest_touch_left : m_action_thumbrest_touch_right;
                const auto& stick_as_dpad = (!wants_swap) ? true_right_joystick_as_dpad : true_left_joystick_as_dpad;
                if (glm::abs(stick_axis) >= snapturn_deadzone){
                    if(!stick_as_dpad) {
                        if (stick_axis < 0) {
                            m_snapturn_left = true;
                        }
                        m_snapturn_on_frame = true;
                    }
                    // Requiring the joystick returning to its natrual position at least once before another snapturn,
                    // even if no snapturn is actually run
                    m_was_snapturn_run_on_input = true; 
                }
            }
        }
        else {
            if (dpad_method == RIGHT_JOYSTICK) {
                if (glm::abs(true_left_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbLY = 0;
                    state->Gamepad.sThumbLX = 0;
                }
            }
            else {
                if (glm::abs(right_joystick_axis.x) < snapturn_deadzone) {
                    m_was_snapturn_run_on_input = false;
                } else {
                    state->Gamepad.sThumbRY = 0;
                    state->Gamepad.sThumbRX = 0;
                }
            }
        }
    }
    
    // Do it again after all the VR buttons have been spoofed
    update_imgui_state_from_xinput_state(*state, true);
}

void VR::on_xinput_set_state(uint32_t* retval, uint32_t user_index, XINPUT_VIBRATION* vibration) {
    ZoneScopedN(__FUNCTION__);

    if (user_index != m_lowest_xinput_user_index) {
        return;
    }

    if (!is_using_controllers()) {
        return;
    }

    const auto left_amplitude = ((float)vibration->wLeftMotorSpeed / 65535.0f) * 5.0f;
    const auto right_amplitude = ((float)vibration->wRightMotorSpeed / 65535.0f) * 5.0f;

    if (left_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, left_amplitude, get_left_joystick());
    }

    if (right_amplitude > 0.0f) {
        trigger_haptic_vibration(0.0f, 0.1f, 1.0f, right_amplitude, get_right_joystick());
    }
}

// Allows imgui navigation to work with the controllers
void VR::update_imgui_state_from_xinput_state(XINPUT_STATE& state, bool is_vr_controller) {
    ZoneScopedN(__FUNCTION__);

    bool is_using_this_controller = true;

    const auto is_using_vr_controller_recently = is_using_controllers_within(std::chrono::seconds(1));
    const auto is_gamepad = !is_vr_controller;

    if (is_vr_controller && !is_using_vr_controller_recently) {
        is_using_this_controller = false;
    } else if (is_gamepad && is_using_vr_controller_recently) { // dont allow gamepad navigation if using vr controllers
        is_using_this_controller = false;
    }

    // L3 + R3 to open the menu
    if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
        if (!FrameworkConfig::get()->is_enable_l3_r3_toggle()) {
            return;
        }

        bool should_open = true;

        const auto now = std::chrono::steady_clock::now();

        if (FrameworkConfig::get()->is_l3_r3_long_press() && !g_framework->is_drawing_ui()) {
            if (!m_xinput_context.menu_longpress_begin_held) {
                m_xinput_context.menu_longpress_begin = now;
            }

            m_xinput_context.menu_longpress_begin_held = true;
            should_open = (now - m_xinput_context.menu_longpress_begin) >= std::chrono::seconds(1);
        } else {
            m_xinput_context.menu_longpress_begin_held = false;
        }

        if (should_open && now - m_last_xinput_l3_r3_menu_open >= std::chrono::seconds(1)) {
            m_last_xinput_l3_r3_menu_open = std::chrono::steady_clock::now();
            g_framework->set_draw_ui(!g_framework->is_drawing_ui());

            state.Gamepad.wButtons &= ~(XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB); // so input doesn't go through to the game
        }
    } else if (is_using_this_controller) {
        m_xinput_context.headlocked_begin_held = false;
        m_xinput_context.menu_longpress_begin_held = false;
    }

    // We need to adjust the stick values based on the selected movement orientation value if the user wants to do this
    // It will either need to be adjusted by the HMD rotation or one of the controllers.
    if (is_using_this_controller && m_movement_orientation->value() != VR::AimMethod::GAME && m_movement_orientation->value() != m_aim_method->value()) {
        const auto left_stick_og = glm::vec2((float)state.Gamepad.sThumbLX, (float)state.Gamepad.sThumbLY );
        const auto left_stick_magnitude = glm::clamp(glm::length(left_stick_og), -32767.0f, 32767.0f);
        const auto left_stick = glm::normalize(left_stick_og);
        const auto left_stick_angle = glm::atan2(left_stick.y, left_stick.x);

        if (this->is_controller_movement_enabled() && is_vr_controller) {
            const auto controller_index = this->get_movement_orientation() == VR::AimMethod::LEFT_CONTROLLER ? get_left_controller_index() : get_right_controller_index();
            const auto controller_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(controller_index)});
            const auto controller_forward = controller_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto controller_angle = glm::atan2(controller_forward.x, controller_forward.z);

            // Normalize angles to [0, 2π]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_controller_angle = controller_angle < 0 ? controller_angle + 2 * glm::pi<float>() : controller_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_controller_angle);
            const auto new_left_stick = glm::vec2(glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)) * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        } else { // Fallback to head aim
            // Rotate the left stick by the HMD rotation
            const auto hmd_rotation = utility::math::flatten(m_rotation_offset * glm::quat{get_rotation(0)});
            const auto hmd_forward = hmd_rotation * glm::vec3(0.0f, 0.0f, 1.0f);
            const auto hmd_angle = glm::atan2(hmd_forward.x, hmd_forward.z);

            // Normalize angles to [0, 2π]
            const auto normalized_left_stick_angle = left_stick_angle < 0 ? left_stick_angle + 2 * glm::pi<float>() : left_stick_angle;
            const auto normalized_hmd_angle = hmd_angle < 0 ? hmd_angle + 2 * glm::pi<float>() : hmd_angle;

            // Add the angles together
            const auto new_left_stick_angle = utility::math::fix_angle(normalized_left_stick_angle + normalized_hmd_angle);
            const auto new_left_stick = glm::vec2{glm::cos(new_left_stick_angle), glm::sin(new_left_stick_angle)} * left_stick_magnitude;

            state.Gamepad.sThumbLX = (int16_t)new_left_stick.x;
            state.Gamepad.sThumbLY = (int16_t)new_left_stick.y;
        }
    }

    if (!g_framework->is_drawing_ui()) {
        m_rt_modifier.draw = false;
        return;
    }

    if (!is_using_this_controller) {
        return;
    }

    // Gamepad navigation when the menu is open
    m_xinput_context.enqueue(is_vr_controller, state, [this](const XINPUT_STATE& state, bool is_vr_controller){
        static auto last_time = std::chrono::high_resolution_clock::now();

        const auto delta = std::chrono::duration<float>((std::chrono::high_resolution_clock::now() - last_time)).count();
        last_time = std::chrono::high_resolution_clock::now();

        auto& io = ImGui::GetIO();
        auto& gamepad = state.Gamepad;

        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        // Headlocked aim toggle
        if (!FrameworkConfig::get()->is_l3_r3_long_press()) {
            if ((state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 && (state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0) {
                if (!m_xinput_context.headlocked_begin_held) {
                    m_xinput_context.headlocked_begin = std::chrono::steady_clock::now();
                    m_xinput_context.headlocked_begin_held = true;
                }
            } else {
                m_xinput_context.headlocked_begin_held = false;
            }
        }

        // Now that we're drawing the UI, check for special button combos the user can use as shortcuts
        // like recenter view, set standing origin, camera offset modification, etc.
        m_rt_modifier.draw = gamepad.bRightTrigger >= 128;

        if (!m_rt_modifier.draw) {
            m_rt_modifier.page = 0;
            m_rt_modifier.was_moving_left = false;
            m_rt_modifier.was_moving_right = false;
        }

        // If user holding down RT with menu open...
        if (m_rt_modifier.draw) {
            // Camera offset modification
            const auto right_ratio = (float)gamepad.sThumbLX / 32767.0f;
            const auto forward_ratio = (float)gamepad.sThumbLY / 32767.0f;
            const auto up_ratio = (float)gamepad.sThumbRY / 32767.0f;

            if (right_ratio <= -0.25f || right_ratio >= 0.25f) {
                const auto right_offset = right_ratio * delta * 150.0f;
                m_camera_right_offset->value() += right_offset;
            }

            if (forward_ratio <= -0.25f || forward_ratio >= 0.25f) {
                const auto forward_offset = forward_ratio * delta * 150.0f;
                m_camera_forward_offset->value() += forward_offset;
            }

            if (up_ratio <= -0.25f || up_ratio >= 0.25f) {
                const auto up_offset = up_ratio * delta * 150.0f;
                m_camera_up_offset->value() += up_offset;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) {
                if (!m_rt_modifier.was_moving_left) {
                    if (m_rt_modifier.page > 0) {
                        m_rt_modifier.page--;
                    } else {
                        m_rt_modifier.page = m_rt_modifier.num_pages - 1;
                    }

                    m_rt_modifier.was_moving_left = true;
                }
            } else {
                m_rt_modifier.was_moving_left = false;
            }

            if (gamepad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) {
                if (!m_rt_modifier.was_moving_right) {
                    if (m_rt_modifier.page < m_rt_modifier.num_pages - 1) {
                        m_rt_modifier.page++;
                    } else {
                        m_rt_modifier.page = 0;
                    }

                    m_rt_modifier.was_moving_right = true;
                }
            } else {
                m_rt_modifier.was_moving_right = false;
            }

            // Reset camera offset
            switch (m_rt_modifier.page) {
            case 2:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    save_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    save_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    save_camera(0);
                }
                break;
            
            case 1:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    load_camera(2);
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    load_camera(1);
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    load_camera(0);
                }

                break; 
            case 0:
            default:
                if (gamepad.wButtons & XINPUT_GAMEPAD_B) {
                    m_camera_right_offset->value() = 0.0f;
                    m_camera_forward_offset->value() = 0.0f;
                    m_camera_up_offset->value() = 0.0f;
                }

                // Recenter
                if (gamepad.wButtons & XINPUT_GAMEPAD_Y) {
                    this->recenter_view();
                }

                // Reset standing origin
                if (gamepad.wButtons & XINPUT_GAMEPAD_X) {
                    this->set_standing_origin(this->get_position(0));
                }
                
                break;
            }

            // ignore everything else
            return;
        }

        // From imgui_impl_win32.cpp
        #define IM_SATURATE(V)                      (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
        #define MAP_BUTTON(KEY_NO, BUTTON_ENUM)     { io.AddKeyEvent(KEY_NO, (gamepad.wButtons & BUTTON_ENUM) != 0); }
        #define MAP_ANALOG(KEY_NO, VALUE, V0, V1)   { float vn = (float)(VALUE - V0) / (float)(V1 - V0); io.AddKeyAnalogEvent(KEY_NO, vn > 0.10f, IM_SATURATE(vn)); }

        MAP_BUTTON(ImGuiKey_GamepadStart,           XINPUT_GAMEPAD_START);
        MAP_BUTTON(ImGuiKey_GamepadBack,            XINPUT_GAMEPAD_BACK);
        MAP_BUTTON(ImGuiKey_GamepadFaceLeft,        XINPUT_GAMEPAD_X);
        MAP_BUTTON(ImGuiKey_GamepadFaceRight,       XINPUT_GAMEPAD_B);
        MAP_BUTTON(ImGuiKey_GamepadFaceUp,          XINPUT_GAMEPAD_Y);
        MAP_BUTTON(ImGuiKey_GamepadFaceDown,        XINPUT_GAMEPAD_A);
        MAP_BUTTON(ImGuiKey_GamepadDpadLeft,        XINPUT_GAMEPAD_DPAD_LEFT);
        MAP_BUTTON(ImGuiKey_GamepadDpadRight,       XINPUT_GAMEPAD_DPAD_RIGHT);
        MAP_BUTTON(ImGuiKey_GamepadDpadUp,          XINPUT_GAMEPAD_DPAD_UP);
        MAP_BUTTON(ImGuiKey_GamepadDpadDown,        XINPUT_GAMEPAD_DPAD_DOWN);
        MAP_ANALOG(ImGuiKey_GamepadL2,              gamepad.bLeftTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_ANALOG(ImGuiKey_GamepadR2,              gamepad.bRightTrigger, XINPUT_GAMEPAD_TRIGGER_THRESHOLD, 255);
        MAP_BUTTON(ImGuiKey_GamepadL3,              XINPUT_GAMEPAD_LEFT_THUMB);
        MAP_BUTTON(ImGuiKey_GamepadR3,              XINPUT_GAMEPAD_RIGHT_THUMB);

        if (!is_vr_controller) {
            MAP_ANALOG(ImGuiKey_GamepadLStickLeft,      gamepad.sThumbLX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_ANALOG(ImGuiKey_GamepadLStickRight,     gamepad.sThumbLX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickUp,        gamepad.sThumbLY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
            MAP_ANALOG(ImGuiKey_GamepadLStickDown,      gamepad.sThumbLY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
            MAP_BUTTON(ImGuiKey_GamepadL1,              XINPUT_GAMEPAD_LEFT_SHOULDER);
            MAP_BUTTON(ImGuiKey_GamepadR1,              XINPUT_GAMEPAD_RIGHT_SHOULDER);
        } else {
            // Map it to the dpad
            const auto left_stick_left = gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_left.was_pressed(left_stick_left)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, true);
            } else if (!left_stick_left) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
            }

            const auto left_stick_right = gamepad.sThumbLX > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_right.was_pressed(left_stick_right)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, true);
            } else if (!left_stick_right) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
            }

            const auto left_stick_up = gamepad.sThumbLY > +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_up.was_pressed(left_stick_up)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, true);
            } else if (!left_stick_up) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadUp, false);
            }

            const auto left_stick_down = gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * 2;
            if (m_xinput_context.vr.left_stick_down.was_pressed(left_stick_down)) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, true);
            } else if (!left_stick_down) {
                io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
            }
        }

        MAP_ANALOG(ImGuiKey_GamepadRStickLeft,      gamepad.sThumbRX, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
        MAP_ANALOG(ImGuiKey_GamepadRStickRight,     gamepad.sThumbRX, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickUp,        gamepad.sThumbRY, +XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, +32767);
        MAP_ANALOG(ImGuiKey_GamepadRStickDown,      gamepad.sThumbRY, -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, -32768);
    });

    // Zero out the state so we don't send input to the game.
    ZeroMemory(&state.Gamepad, sizeof(XINPUT_GAMEPAD));
}

void VR::record_hitch_snapshot_sample(std::chrono::steady_clock::time_point now) {
    auto& sample = m_hitch_snapshot_samples[m_hitch_snapshot_cursor];
    sample = {};
    sample.timestamp = now;
    sample.sequence = ++m_hitch_snapshot_sequence;
    sample.frame_count = m_frame_count;
    sample.render_frame_count = m_render_frame_count;
    sample.rendering_method = m_rendering_method != nullptr ? m_rendering_method->value() : -1;
    sample.hmd_active = is_hmd_active();
    sample.runtime_loaded = get_runtime() != nullptr && get_runtime()->loaded;
    sample.runtime_ready = get_runtime() != nullptr && get_runtime()->ready();
    sample.using_controllers = is_using_controllers();
    sample.using_afr = is_using_afr();
    sample.native_stereo_fix = is_native_stereo_fix_enabled();
    sample.submitted = m_submitted;
    sample.framework_frame_age_ms = g_framework == nullptr ? -1 : hitch_age_ms(now, g_framework->get_last_framework_on_frame_time());
    sample.mod_frame_age_ms = hitch_age_ms(now, m_last_mod_frame);
    sample.d3d12_frame_age_ms = hitch_age_ms(now, m_d3d12.get_last_on_frame_time());
    sample.cvar_change_counter = m_cvar_manager != nullptr ? m_cvar_manager->get_change_counter() : 0;
    sample.d3d12 = m_is_d3d12 ? m_d3d12.get_hitch_frame_snapshot(this) : vrmod::D3D12Component::HitchFrameSnapshot{};

    if (const auto runtime = get_runtime(); runtime != nullptr && runtime->is_openxr()) {
        if (const auto openxr = get_openxr_runtime(); openxr != nullptr) {
            sample.xr_wait_age_ms = hitch_age_ms(now, openxr->last_successful_wait_frame);
            sample.xr_begin_age_ms = hitch_age_ms(now, openxr->last_successful_begin_frame);
            sample.xr_end_age_ms = hitch_age_ms(now, openxr->last_successful_end_frame);
            sample.pose_update_age_ms = hitch_age_ms(now, openxr->last_successful_pose_update);
            sample.session_state = (int)openxr->session_state;
            sample.session_ready = openxr->session_ready;
            sample.frame_synced = openxr->frame_synced;
            sample.frame_began = openxr->frame_began;
            sample.got_first_poses = openxr->got_first_poses;
            sample.got_first_valid_poses = openxr->got_first_valid_poses;
            sample.accepted_relaxed_startup_poses = openxr->accepted_relaxed_startup_poses;
            sample.stale_pose_older_frame_skip_count = openxr->stale_pose_older_frame_skip_count;
            sample.stale_pose_older_frame_skip_suppressed_count = openxr->stale_pose_skip_suppressed_total;
            sample.stale_pose_refresh_attempt_count = openxr->stale_pose_refresh_attempt_count;
            sample.stale_pose_refresh_success_count = openxr->stale_pose_refresh_success_count;
            sample.stale_pose_refresh_failed_count = openxr->stale_pose_refresh_failed_count;
            sample.focused_frame_loop_recovery_count = openxr->focused_frame_loop_recovery_count;
            sample.forced_frame_recovery_count = openxr->forced_frame_recovery_count;
            sample.last_pose_update_frame_count = openxr->last_pose_update_frame_count;
        }
    }

    if (m_fake_stereo_hook != nullptr) {
        if (const auto rtm = m_fake_stereo_hook->get_render_target_manager(); rtm != nullptr) {
            sample.scene_capture_destroyed_count = rtm->get_scene_capture_destroyed_count();
        }
    }

    m_hitch_snapshot_cursor = (m_hitch_snapshot_cursor + 1) % HITCH_SNAPSHOT_RING_SIZE;
    if (m_hitch_snapshot_cursor == 0) {
        m_hitch_snapshot_wrapped = true;
    }
}

void VR::enqueue_hitch_snapshot_dump(HitchSnapshotDumpRequest&& request) {
    {
        std::scoped_lock lock{m_hitch_snapshot_writer_mutex};

        if (!m_hitch_snapshot_writer_thread.joinable()) {
            m_hitch_snapshot_writer_thread = std::jthread([this](std::stop_token stop_token) {
                hitch_snapshot_writer_loop(stop_token);
            });
        }

        while (m_hitch_snapshot_dump_queue.size() >= HITCH_SNAPSHOT_MAX_PENDING_DUMPS) {
            m_hitch_snapshot_dump_queue.pop_front();
        }

        m_hitch_snapshot_dump_queue.emplace_back(std::move(request));
    }

    m_hitch_snapshot_writer_cv.notify_one();
}

void VR::hitch_snapshot_writer_loop(std::stop_token stop_token) {
    while (true) {
        HitchSnapshotDumpRequest request{};

        {
            std::unique_lock lock{m_hitch_snapshot_writer_mutex};
            m_hitch_snapshot_writer_cv.wait(lock, [this, &stop_token]() {
                return stop_token.stop_requested() || !m_hitch_snapshot_dump_queue.empty();
            });

            if (stop_token.stop_requested()) {
                m_hitch_snapshot_dump_queue.clear();
                return;
            }

            request = std::move(m_hitch_snapshot_dump_queue.front());
            m_hitch_snapshot_dump_queue.pop_front();
        }

        write_hitch_snapshot_request(std::move(request));
    }
}

void VR::stop_hitch_snapshot_writer() {
    if (!m_hitch_snapshot_writer_thread.joinable()) {
        return;
    }

    m_hitch_snapshot_writer_thread.request_stop();
    m_hitch_snapshot_writer_cv.notify_all();
    m_hitch_snapshot_writer_thread.join();

    std::scoped_lock lock{m_hitch_snapshot_writer_mutex};
    m_hitch_snapshot_dump_queue.clear();
}

void VR::write_hitch_snapshot_request(HitchSnapshotDumpRequest&& request) try {
    std::filesystem::create_directories(request.path.parent_path());

    json samples = json::array();
    HitchSnapshotSample last_valid_sample{};
    bool has_last_valid_sample = false;

    for (const auto& sample : request.samples) {
        if (sample.timestamp.time_since_epoch().count() == 0) {
            continue;
        }

        last_valid_sample = sample;
        has_last_valid_sample = true;
        const auto& d3d12 = sample.d3d12;
        samples.push_back({
            {"age_ms", hitch_age_ms(request.dump_time, sample.timestamp)},
            {"sequence", sample.sequence},
            {"frame_count", sample.frame_count},
            {"render_frame_count", sample.render_frame_count},
            {"rendering_method", sample.rendering_method},
            {"hmd_active", sample.hmd_active},
            {"runtime_loaded", sample.runtime_loaded},
            {"runtime_ready", sample.runtime_ready},
            {"using_controllers", sample.using_controllers},
            {"using_afr", sample.using_afr},
            {"native_stereo_fix", sample.native_stereo_fix},
            {"submitted", sample.submitted},
            {"framework_frame_age_ms", sample.framework_frame_age_ms},
            {"mod_frame_age_ms", sample.mod_frame_age_ms},
            {"d3d12_frame_age_ms", sample.d3d12_frame_age_ms},
            {"xr_wait_age_ms", sample.xr_wait_age_ms},
            {"xr_begin_age_ms", sample.xr_begin_age_ms},
            {"xr_end_age_ms", sample.xr_end_age_ms},
            {"pose_update_age_ms", sample.pose_update_age_ms},
            {"session_state", sample.session_state},
            {"session_ready", sample.session_ready},
            {"frame_synced", sample.frame_synced},
            {"frame_began", sample.frame_began},
            {"got_first_poses", sample.got_first_poses},
            {"got_first_valid_poses", sample.got_first_valid_poses},
            {"accepted_relaxed_startup_poses", sample.accepted_relaxed_startup_poses},
            {"stale_pose_older_frame_skip_count", sample.stale_pose_older_frame_skip_count},
            {"stale_pose_older_frame_skip_suppressed_count", sample.stale_pose_older_frame_skip_suppressed_count},
            {"stale_pose_refresh_attempt_count", sample.stale_pose_refresh_attempt_count},
            {"stale_pose_refresh_success_count", sample.stale_pose_refresh_success_count},
            {"stale_pose_refresh_failed_count", sample.stale_pose_refresh_failed_count},
            {"focused_frame_loop_recovery_count", sample.focused_frame_loop_recovery_count},
            {"forced_frame_recovery_count", sample.forced_frame_recovery_count},
            {"last_pose_update_frame_count", sample.last_pose_update_frame_count},
            {"scene_capture_destroyed_count", sample.scene_capture_destroyed_count},
            {"cvar_change_counter", sample.cvar_change_counter},
            {"d3d12", {
                {"initialized", d3d12.initialized},
                {"force_reset", d3d12.force_reset},
                {"last_afr_state", d3d12.last_afr_state},
                {"has_prev_backbuffer", d3d12.has_prev_backbuffer},
                {"has_game_tex", d3d12.has_game_tex},
                {"has_ui_tex", d3d12.has_ui_tex},
                {"has_scene_capture_tex", d3d12.has_scene_capture_tex},
                {"backbuffer_width", d3d12.backbuffer_width},
                {"backbuffer_height", d3d12.backbuffer_height},
                {"ui_extent_width", d3d12.ui_extent_width},
                {"ui_extent_height", d3d12.ui_extent_height},
                {"hmd_width", d3d12.hmd_width},
                {"hmd_height", d3d12.hmd_height},
                {"openxr_swapchain_count", d3d12.openxr_swapchain_count},
                {"ui_swapchain_width", d3d12.ui_swapchain_width},
                {"ui_swapchain_height", d3d12.ui_swapchain_height},
                {"eye_swapchain_width", d3d12.eye_swapchain_width},
                {"eye_swapchain_height", d3d12.eye_swapchain_height},
                {"depth_swapchain_width", d3d12.depth_swapchain_width},
                {"depth_swapchain_height", d3d12.depth_swapchain_height},
                {"swapchain_recreate_count", d3d12.swapchain_recreate_count},
                {"last_swapchain_recreate_reasons", d3d12.last_swapchain_recreate_reasons},
                {"perf_on_frame_count", d3d12.perf_on_frame_count},
                {"perf_on_frame_avg_ms", d3d12.perf_on_frame_avg_ms},
                {"perf_on_frame_max_ms", d3d12.perf_on_frame_max_ms},
                {"perf_ui_copy_count", d3d12.perf_ui_copy_count},
                {"perf_ui_copy_avg_ms", d3d12.perf_ui_copy_avg_ms},
                {"perf_ui_copy_max_ms", d3d12.perf_ui_copy_max_ms},
                {"perf_swapchain_copy_count", d3d12.perf_swapchain_copy_count},
                {"perf_swapchain_copy_avg_ms", d3d12.perf_swapchain_copy_avg_ms},
                {"perf_swapchain_copy_max_ms", d3d12.perf_swapchain_copy_max_ms},
                {"perf_openxr_submit_count", d3d12.perf_openxr_submit_count},
                {"perf_openxr_submit_avg_ms", d3d12.perf_openxr_submit_avg_ms},
                {"perf_openxr_submit_max_ms", d3d12.perf_openxr_submit_max_ms},
            }},
        });
    }

    const auto& latest_cvar_change = request.latest_cvar_change;
    json root{
        {"type", "uevr_hitch_snapshot"},
        {"tick_gap_ms", request.tick_gap_ms},
        {"suspected_stall", request.suspected_stall},
        {"sample_count", samples.size()},
        {"latest_cvar_change", {
            {"counter", latest_cvar_change.counter},
            {"name", latest_cvar_change.name},
            {"value", latest_cvar_change.value},
            {"source", latest_cvar_change.source},
        }},
        {"samples", std::move(samples)},
    };

    std::ofstream file{request.path};
    file << root.dump(2);

    if (has_last_valid_sample) {
        const auto& sample = last_valid_sample;
        const auto& d3d12 = sample.d3d12;
        json summary{
            {"type", "uevr_hitch_snapshot_summary"},
            {"source", request.path.filename().string()},
            {"tick_gap_ms", request.tick_gap_ms},
            {"suspected_stall", request.suspected_stall},
            {"sample_count", root["sample_count"].get<size_t>()},
            {"last_sample", {
                {"frame_count", sample.frame_count},
                {"render_frame_count", sample.render_frame_count},
                {"runtime_ready", sample.runtime_ready},
                {"using_controllers", sample.using_controllers},
                {"xr_wait_age_ms", sample.xr_wait_age_ms},
                {"xr_begin_age_ms", sample.xr_begin_age_ms},
                {"xr_end_age_ms", sample.xr_end_age_ms},
                {"pose_update_age_ms", sample.pose_update_age_ms},
                {"frame_synced", sample.frame_synced},
                {"frame_began", sample.frame_began},
                {"stale_pose_older_frame_skip_count", sample.stale_pose_older_frame_skip_count},
                {"stale_pose_refresh_attempt_count", sample.stale_pose_refresh_attempt_count},
                {"stale_pose_refresh_success_count", sample.stale_pose_refresh_success_count},
                {"stale_pose_refresh_failed_count", sample.stale_pose_refresh_failed_count},
                {"focused_frame_loop_recovery_count", sample.focused_frame_loop_recovery_count},
                {"forced_frame_recovery_count", sample.forced_frame_recovery_count},
                {"scene_capture_destroyed_count", sample.scene_capture_destroyed_count},
            }},
            {"d3d12", {
                {"has_game_tex", d3d12.has_game_tex},
                {"has_ui_tex", d3d12.has_ui_tex},
                {"has_scene_capture_tex", d3d12.has_scene_capture_tex},
                {"swapchain_recreate_count", d3d12.swapchain_recreate_count},
                {"last_swapchain_recreate_reasons", d3d12.last_swapchain_recreate_reasons},
                {"perf_on_frame_avg_ms", d3d12.perf_on_frame_avg_ms},
                {"perf_on_frame_max_ms", d3d12.perf_on_frame_max_ms},
                {"perf_openxr_submit_avg_ms", d3d12.perf_openxr_submit_avg_ms},
                {"perf_openxr_submit_max_ms", d3d12.perf_openxr_submit_max_ms},
            }},
        };

        std::ofstream summary_file{request.path.parent_path() / "hitch_snapshot_latest_summary.json"};
        summary_file << summary.dump(2);
    }

    prune_hitch_snapshots(request.path.parent_path());
    SPDLOG_INFO("[VR][hitch-snapshot] Wrote {}", request.path.string());
} catch (const std::exception& e) {
    SPDLOG_WARN("[VR][hitch-snapshot] Failed to write snapshot: {}", e.what());
} catch (...) {
    SPDLOG_WARN("[VR][hitch-snapshot] Failed to write snapshot");
}

void VR::prune_hitch_snapshots(const std::filesystem::path& dir) {
    try {
        if (!std::filesystem::exists(dir)) {
            return;
        }

        std::vector<std::filesystem::directory_entry> snapshots{};

        for (const auto& entry : std::filesystem::directory_iterator{dir}) {
            if (!entry.is_regular_file()) {
                continue;
            }

            const auto filename = entry.path().filename().string();
            if (filename.starts_with("hitch_snapshot_") && filename.ends_with(".json") &&
                filename != "hitch_snapshot_latest_summary.json")
            {
                snapshots.push_back(entry);
            }
        }

        if (snapshots.size() <= HITCH_SNAPSHOT_MAX_FILES) {
            return;
        }

        std::sort(snapshots.begin(), snapshots.end(), [](const auto& a, const auto& b) {
            return a.last_write_time() < b.last_write_time();
        });

        const auto remove_count = snapshots.size() - HITCH_SNAPSHOT_MAX_FILES;
        for (size_t i = 0; i < remove_count; ++i) {
            std::error_code ec{};
            std::filesystem::remove(snapshots[i].path(), ec);
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("[VR][hitch-snapshot] Failed to prune old snapshots: {}", e.what());
    } catch (...) {
        SPDLOG_WARN("[VR][hitch-snapshot] Failed to prune old snapshots");
    }
}

void VR::dump_hitch_snapshot(std::chrono::steady_clock::duration tick_gap, const char* suspected_stall, bool bypass_cooldown) try {
    if (!m_enable_hitch_diagnostics->value()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();

    if (!bypass_cooldown &&
        m_last_hitch_snapshot_dump.time_since_epoch().count() != 0 &&
        now - m_last_hitch_snapshot_dump < std::chrono::seconds(30))
    {
        return;
    }

    m_last_hitch_snapshot_dump = now;
    const auto dir = Framework::get_persistent_dir("hitch_snapshots");
    const auto path = dir / std::format(
        "hitch_snapshot_{}_{}.json",
        hitch_timestamp_suffix(),
        ++m_hitch_snapshot_dump_count);

    const auto count = m_hitch_snapshot_wrapped ? HITCH_SNAPSHOT_RING_SIZE : m_hitch_snapshot_cursor;
    const auto start = m_hitch_snapshot_wrapped ? m_hitch_snapshot_cursor : 0;
    HitchSnapshotDumpRequest request{};
    request.path = path;
    request.dump_time = now;
    request.tick_gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(tick_gap).count();
    request.suspected_stall = suspected_stall != nullptr ? suspected_stall : "unknown";
    request.latest_cvar_change = m_cvar_manager != nullptr ? m_cvar_manager->get_change_snapshot() : CVarManager::ChangeSnapshot{};
    request.samples.reserve(count);

    HitchSnapshotSample last_valid_sample{};
    bool has_last_valid_sample = false;

    for (size_t i = 0; i < count; ++i) {
        const auto& sample = m_hitch_snapshot_samples[(start + i) % HITCH_SNAPSHOT_RING_SIZE];

        if (sample.timestamp.time_since_epoch().count() == 0) {
            continue;
        }

        last_valid_sample = sample;
        has_last_valid_sample = true;
        request.samples.push_back(sample);
    }

    if (has_last_valid_sample) {
        m_last_hitch_snapshot_summary = HitchSnapshotSummary{
            .valid = true,
            .path = path,
            .timestamp = now,
            .tick_gap_ms = request.tick_gap_ms,
            .suspected_stall = request.suspected_stall,
            .sample_count = request.samples.size(),
            .last_sample = last_valid_sample,
        };
    }

    enqueue_hitch_snapshot_dump(std::move(request));
} catch (const std::exception& e) {
    SPDLOG_WARN("[VR][hitch-snapshot] Failed to queue snapshot: {}", e.what());
} catch (...) {
    SPDLOG_WARN("[VR][hitch-snapshot] Failed to queue snapshot");
}

void VR::note_stalker2_transition_stress(const char* reason) {
    if (!m_is_d3d12 || m_openxr == nullptr || get_runtime() == nullptr ||
        !get_runtime()->is_openxr() || !is_stalker2_executable_cached() ||
        !m_openxr->got_first_valid_poses)
    {
        return;
    }

    constexpr auto STRESS_HOLD = std::chrono::milliseconds{350};
    constexpr auto NEW_BURST_GAP_MS = 1000LL;

    const auto now = std::chrono::steady_clock::now();
    const auto now_ms = steady_clock_ms(now);
    const auto previous_stress_ms = m_stalker2_transition_last_stress_ms.exchange(now_ms);

    if (previous_stress_ms == 0 || now_ms - previous_stress_ms > NEW_BURST_GAP_MS) {
        m_stalker2_transition_first_stress_ms.store(now_ms);
        m_stalker2_transition_last_defer_ms.store(0);
        m_stalker2_transition_deferred_frames.store(0);
    }

    const auto until_ms = steady_clock_ms(now + STRESS_HOLD);
    auto current_until_ms = m_stalker2_transition_stress_until_ms.load();

    while (until_ms > current_until_ms &&
        !m_stalker2_transition_stress_until_ms.compare_exchange_weak(current_until_ms, until_ms))
    {
    }

    const auto events = m_stalker2_transition_stress_events.fetch_add(1) + 1;

    SPDLOG_INFO_EVERY_N_SEC(
        2,
        "[Stalker2][OpenXR] Transition stress noted reason={} events={} hold_until_ms={}",
        reason != nullptr ? reason : "unknown",
        events,
        m_stalker2_transition_stress_until_ms.load());
}

bool VR::should_defer_stalker2_openxr_frame_for_transition(const char* reason) {
    if (!m_is_d3d12 || m_openxr == nullptr || get_runtime() == nullptr ||
        !get_runtime()->is_openxr() || !is_stalker2_executable_cached() ||
        !m_openxr->can_run_frame_loop() || !m_openxr->got_first_valid_poses)
    {
        return false;
    }

    if (!STALKER2_TRANSITION_OPENXR_DEFERS_ENABLED) {
        SPDLOG_INFO_ONCE(
            "[Stalker2][OpenXR] Transition defer guard disabled for A/B; leaving stable RT copy and stress diagnostics active");
        return false;
    }

    // Never interfere with an already-open or already-waited frame. The guard is
    // only for avoiding a new blocking xrWaitFrame while UE5.1 is in a known
    // Stalker2 cutscene/gameplay render-target transition.
    if (m_openxr->frame_began || m_openxr->frame_synced) {
        return false;
    }

    constexpr auto MAX_BURST_DEFER_MS = 700LL;
    constexpr auto MIN_DEFER_SPACING_MS = 16LL;
    constexpr auto MAX_DEFERRED_FRAMES_PER_BURST = 1u;
    constexpr auto MAX_LAST_END_AGE_MS = 250LL;

    const auto now_ms = steady_clock_ms();
    const auto until_ms = m_stalker2_transition_stress_until_ms.load();
    const auto now = std::chrono::steady_clock::now();

    if (until_ms == 0 || now_ms > until_ms) {
        return false;
    }

    if (!m_openxr->ever_submitted ||
        m_openxr->last_successful_end_frame.time_since_epoch().count() == 0 ||
        std::chrono::duration_cast<std::chrono::milliseconds>(now - m_openxr->last_successful_end_frame).count() > MAX_LAST_END_AGE_MS)
    {
        return false;
    }

    const auto first_stress_ms = m_stalker2_transition_first_stress_ms.load();

    if (first_stress_ms == 0 || now_ms - first_stress_ms > MAX_BURST_DEFER_MS) {
        return false;
    }

    if (m_stalker2_transition_deferred_frames.load() >= MAX_DEFERRED_FRAMES_PER_BURST) {
        return false;
    }

    const auto previous_defer_ms = m_stalker2_transition_last_defer_ms.exchange(now_ms);

    if (previous_defer_ms != 0 && now_ms - previous_defer_ms < MIN_DEFER_SPACING_MS) {
        return false;
    }

    const auto deferred = m_stalker2_transition_deferred_frames.fetch_add(1) + 1;

    SPDLOG_WARNING_EVERY_N_SEC(
        1,
        "[Stalker2][OpenXR] Deferring one D3D12 OpenXR submit during transition stress reason={} deferred={} until_ms={} first_stress_age_ms={}",
        reason != nullptr ? reason : "unknown",
        deferred,
        until_ms,
        now_ms - first_stress_ms);

    return true;
}

void VR::on_pre_engine_tick(sdk::UGameEngine* engine, float delta) {
    ZoneScopedN(__FUNCTION__);

    const auto now = std::chrono::steady_clock::now();
    const auto previous_engine_tick = m_last_engine_tick;
    const bool hitch_diagnostics_enabled = m_enable_hitch_diagnostics->value();

    m_cvar_manager->on_pre_engine_tick(engine, delta);
    if (!hitch_diagnostics_enabled) {
        if (m_hitch_diagnostics_enabled_last_frame) {
            stop_hitch_snapshot_writer();
            m_hitch_snapshot_cursor = 0;
            m_hitch_snapshot_wrapped = false;
            m_last_hitch_snapshot_sample = {};
        }

        m_hitch_diagnostics_enabled_last_frame = false;
    } else if (m_last_hitch_snapshot_sample.time_since_epoch().count() == 0 ||
        now - m_last_hitch_snapshot_sample >= HITCH_SNAPSHOT_SAMPLE_INTERVAL)
    {
        m_hitch_diagnostics_enabled_last_frame = true;
        record_hitch_snapshot_sample(now);
        m_last_hitch_snapshot_sample = now;
    }
    m_last_engine_tick = now;

    if (hitch_diagnostics_enabled && previous_engine_tick.time_since_epoch().count() != 0) {
        const auto tick_gap = now - previous_engine_tick;

        if (tick_gap > std::chrono::milliseconds(250) &&
            (m_last_tick_gap_log.time_since_epoch().count() == 0 || now - m_last_tick_gap_log >= std::chrono::seconds(1)))
        {
            m_last_tick_gap_log = now;

            if (const auto runtime = get_runtime(); runtime != nullptr && runtime->is_openxr()) {
                if (const auto openxr = get_openxr_runtime(); openxr != nullptr) {
                    const auto mod_frame_gap_ms = m_last_mod_frame.time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_mod_frame).count();
                    const auto framework_frame_gap_ms = (g_framework == nullptr || g_framework->get_last_framework_on_frame_time().time_since_epoch().count() == 0)
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - g_framework->get_last_framework_on_frame_time()).count();
                    const auto d3d12_frame_gap_ms = m_d3d12.get_last_on_frame_time().time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - m_d3d12.get_last_on_frame_time()).count();
                    const auto xr_begin_gap_ms = openxr->last_successful_begin_frame.time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - openxr->last_successful_begin_frame).count();
                    const auto xr_end_gap_ms = openxr->last_successful_end_frame.time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - openxr->last_successful_end_frame).count();
                    const auto xr_wait_gap_ms = openxr->last_successful_wait_frame.time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - openxr->last_successful_wait_frame).count();
                    const auto pose_update_gap_ms = openxr->last_successful_pose_update.time_since_epoch().count() == 0
                        ? -1ll
                        : std::chrono::duration_cast<std::chrono::milliseconds>(now - openxr->last_successful_pose_update).count();

                    if (openxr->session_state == XR_SESSION_STATE_FOCUSED) {
                        ++m_post_focus_tick_gap_count;

                        if (tick_gap > std::chrono::seconds(1)) {
                            ++m_post_focus_long_tick_gap_count;
                        }
                    }

                    spdlog::warn(
                        "[VR] Large engine tick gap detected: {} ms. mod_frame={}ms framework_frame={}ms d3d12_frame={}ms xrBegin={}ms xrEnd={}ms session_state={} session_ready={} frame_synced={} frame_began={} got_first_poses={} got_first_valid_poses={} post_focus_gaps={} post_focus_long_gaps={}",
                        std::chrono::duration_cast<std::chrono::milliseconds>(tick_gap).count(),
                        mod_frame_gap_ms,
                        framework_frame_gap_ms,
                        d3d12_frame_gap_ms,
                        xr_begin_gap_ms,
                        xr_end_gap_ms,
                        openxr->get_session_state_string(openxr->session_state),
                        openxr->session_ready,
                        openxr->frame_synced,
                        openxr->frame_began,
                        openxr->got_first_poses,
                        openxr->got_first_valid_poses,
                        m_post_focus_tick_gap_count,
                        m_post_focus_long_tick_gap_count
                    );

                    if (openxr->session_state == XR_SESSION_STATE_FOCUSED) {
                        const char* suspected_stall = "mixed_or_unknown";

                        if (mod_frame_gap_ms > 500 && framework_frame_gap_ms <= 250 && d3d12_frame_gap_ms <= 250 && xr_wait_gap_ms <= 250 && xr_begin_gap_ms <= 250 && xr_end_gap_ms <= 250) {
                            suspected_stall = "game_tick_starved_mod_frame_only";
                        } else if (mod_frame_gap_ms > 500 && framework_frame_gap_ms > 500 && d3d12_frame_gap_ms <= 250 && xr_wait_gap_ms <= 250 && xr_begin_gap_ms <= 250 && xr_end_gap_ms <= 250) {
                            suspected_stall = "game_or_framework_tick_starved_while_render_runtime_still_advancing";
                        } else if (d3d12_frame_gap_ms > 500 && xr_begin_gap_ms <= 250 && xr_end_gap_ms <= 250) {
                            suspected_stall = "d3d12_component_not_advancing";
                        } else if (xr_wait_gap_ms > 500 && xr_begin_gap_ms > 500 && xr_end_gap_ms > 500) {
                            suspected_stall = "openxr_frame_loop_not_advancing";
                        } else if (pose_update_gap_ms > 500 && xr_wait_gap_ms <= 250) {
                            suspected_stall = "pose_updates_not_advancing";
                        }

                        spdlog::warn(
                            "[VR][stall-detail] tick={}ms mod_frame={}ms framework_frame={}ms d3d12_frame={}ms xrWait={}ms xrBegin={}ms xrEnd={}ms pose_update={}ms accepted_relaxed_startup_poses={} suspected={}",
                            std::chrono::duration_cast<std::chrono::milliseconds>(tick_gap).count(),
                            mod_frame_gap_ms,
                            framework_frame_gap_ms,
                            d3d12_frame_gap_ms,
                            xr_wait_gap_ms,
                            xr_begin_gap_ms,
                            xr_end_gap_ms,
                            pose_update_gap_ms,
                            openxr->accepted_relaxed_startup_poses,
                            suspected_stall
                        );

                        if (is_prospi_executable() && tick_gap <= std::chrono::seconds(1)) {
                            constexpr auto rolling_window = std::chrono::seconds{10};
                            constexpr size_t rolling_threshold = 3;

                            m_prospi_rolling_hitch_gaps[m_prospi_rolling_hitch_gap_cursor] = now;
                            m_prospi_rolling_hitch_gap_cursor = (m_prospi_rolling_hitch_gap_cursor + 1) % PROSPI_ROLLING_HITCH_GAP_RING_SIZE;
                            if (m_prospi_rolling_hitch_gap_cursor == 0) {
                                m_prospi_rolling_hitch_gap_wrapped = true;
                            }

                            size_t rolling_count = 0;
                            const auto rolling_sample_count = m_prospi_rolling_hitch_gap_wrapped
                                ? PROSPI_ROLLING_HITCH_GAP_RING_SIZE
                                : m_prospi_rolling_hitch_gap_cursor;

                            for (size_t i = 0; i < rolling_sample_count; ++i) {
                                const auto& sample_time = m_prospi_rolling_hitch_gaps[i];
                                if (sample_time.time_since_epoch().count() != 0 && now - sample_time <= rolling_window) {
                                    ++rolling_count;
                                }
                            }

                            if (rolling_count >= rolling_threshold &&
                                (m_last_prospi_rolling_hitch_snapshot.time_since_epoch().count() == 0 ||
                                    now - m_last_prospi_rolling_hitch_snapshot >= rolling_window))
                            {
                                m_last_prospi_rolling_hitch_snapshot = now;
                                spdlog::warn(
                                    "[PROSPI_HITCH] rolling tick-gap trigger: count={} window={}s latest={}ms suspected={}",
                                    rolling_count,
                                    std::chrono::duration_cast<std::chrono::seconds>(rolling_window).count(),
                                    std::chrono::duration_cast<std::chrono::milliseconds>(tick_gap).count(),
                                    suspected_stall);
                                dump_hitch_snapshot(tick_gap, "prospi_rolling_tick_gaps", true);
                            }
                        }

                        const auto openxr_frame_loop_candidate =
                            openxr->session_ready &&
                            openxr->frame_synced &&
                            !openxr->frame_began &&
                            openxr->got_first_poses &&
                            openxr->got_first_valid_poses &&
                            m_d3d12.has_game_and_ui_textures() &&
                            xr_wait_gap_ms > 1000 &&
                            xr_begin_gap_ms > 1000 &&
                            xr_end_gap_ms > 1000;

                        if (openxr_frame_loop_candidate &&
                            (m_last_openxr_recovery_candidate_log.time_since_epoch().count() == 0 ||
                                now - m_last_openxr_recovery_candidate_log >= std::chrono::seconds(10)))
                        {
                            m_last_openxr_recovery_candidate_log = now;
                            spdlog::warn(
                                "[VR][recovery-candidate] Focused OpenXR frame loop is stale with frame_synced=true and frame_began=false; D3D12 textures are present. wait={}ms begin={}ms end={}ms pose={}ms stale_skips={} stale_refresh={}/{}/{} recoveries={}/{}",
                                xr_wait_gap_ms,
                                xr_begin_gap_ms,
                                xr_end_gap_ms,
                                pose_update_gap_ms,
                                openxr->stale_pose_older_frame_skip_count,
                                openxr->stale_pose_refresh_attempt_count,
                                openxr->stale_pose_refresh_success_count,
                                openxr->stale_pose_refresh_failed_count,
                                openxr->focused_frame_loop_recovery_count,
                                openxr->forced_frame_recovery_count);
                        }

                        if (tick_gap > std::chrono::seconds(1)) {
                            dump_hitch_snapshot(tick_gap, suspected_stall);
                        }
                    } else if (tick_gap > std::chrono::seconds(2)) {
                        dump_hitch_snapshot(tick_gap, "non_focused_or_unknown");
                    }
                }
            }
        }
    }

    if (!get_runtime()->loaded || !is_hmd_active()) {
        return;
    }

    SPDLOG_INFO_ONCE("VR: Pre-engine tick");

    m_render_target_pool_hook->on_pre_engine_tick(engine, delta);

    update_statistics_overlay(engine);
    update_game_fov();
    update_shf_auto_2d_mode(engine);
    update_dispatch_auto_2d_mode(engine);

    // Dont update action states on AFR frames
    // TODO: fix this for actual AFR, but we dont really care about pure AFR since synced beats it most of the time
    if (m_fake_stereo_hook != nullptr && !m_fake_stereo_hook->is_ignoring_next_viewport_draw()) {
        update_action_states();
    }
}

void VR::update_shf_auto_2d_mode(sdk::UGameEngine* engine) {
    if (!is_shf_executable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_shf_auto_2d_last_sample.time_since_epoch().count() != 0 &&
        now - m_shf_auto_2d_last_sample < std::chrono::milliseconds(250)) {
        return;
    }

    m_shf_auto_2d_last_sample = now;

    const auto decision = evaluate_shf_auto_2d(engine);

    if (decision.should_force) {
        if (!m_shf_auto_2d_active) {
            m_shf_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_shf_auto_2d_active = true;
            spdlog::info(
                "[SHf][Auto2D] active=true previous={} cutscene={} fov={:.3f} url={} target={}",
                m_shf_auto_2d_previous_mode,
                utility::narrow(decision.cutscene),
                decision.fov.value_or(0.0f),
                utility::narrow(decision.url),
                decision.target.empty() ? "unresolved" : utility::narrow(decision.target));
        }

        m_2d_screen_mode->value() = true;
        return;
    }

    if (m_shf_auto_2d_active) {
        m_2d_screen_mode->value() = m_shf_auto_2d_previous_mode;
        m_shf_auto_2d_active = false;
        spdlog::info(
            "[SHf][Auto2D] active=false restored={} cutscene={} fov={} url={} target={}",
            m_shf_auto_2d_previous_mode,
            utility::narrow(decision.cutscene),
            decision.fov.has_value() ? std::format("{:.3f}", *decision.fov) : "unresolved",
            decision.url.empty() ? "unresolved" : utility::narrow(decision.url),
            decision.target.empty() ? "unresolved" : utility::narrow(decision.target));
    }
}

void VR::update_dispatch_auto_2d_mode(sdk::UGameEngine* engine) {
    if (!is_dispatch_executable()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (m_dispatch_auto_2d_last_sample.time_since_epoch().count() != 0 &&
        now - m_dispatch_auto_2d_last_sample < std::chrono::milliseconds(250)) {
        return;
    }

    m_dispatch_auto_2d_last_sample = now;

    const auto decision = evaluate_dispatch_auto_2d(engine);

    if (decision.should_force) {
        if (!m_dispatch_auto_2d_active) {
            m_dispatch_auto_2d_previous_mode = m_2d_screen_mode->value();
            m_dispatch_auto_2d_active = true;
            spdlog::info(
                "[Dispatch][Auto2D] active=true previous={} reason={} subsystem={} source={} player={} texture={} playing={} preparing={} buffering={} ready={}",
                m_dispatch_auto_2d_previous_mode,
                decision.reason,
                decision.subsystem.empty() ? "unresolved" : decision.subsystem,
                decision.source.empty() ? "unresolved" : decision.source,
                decision.player.empty() ? "unresolved" : decision.player,
                decision.texture.empty() ? "unresolved" : decision.texture,
                decision.playing.has_value() ? (*decision.playing ? "true" : "false") : "unresolved",
                decision.preparing.has_value() ? (*decision.preparing ? "true" : "false") : "unresolved",
                decision.buffering.has_value() ? (*decision.buffering ? "true" : "false") : "unresolved",
                decision.ready.has_value() ? (*decision.ready ? "true" : "false") : "unresolved");
        }

        m_2d_screen_mode->value() = true;
        return;
    }

    if (m_dispatch_auto_2d_active) {
        m_2d_screen_mode->value() = m_dispatch_auto_2d_previous_mode;
        m_dispatch_auto_2d_active = false;
        spdlog::info("[Dispatch][Auto2D] active=false restored={}", m_dispatch_auto_2d_previous_mode);
    }
}

void VR::update_game_fov() {
    struct ProSpiTelephotoPerfTarget {
        float view_distance_scale{1.0f};
        float static_mesh_lod_distance_scale{1.0f};
        int skeletal_mesh_lod_bias{0};
        bool line_mode{false};
    };

    const auto update_prospi_telephoto_perf_override = [&](bool should_apply, const ProSpiTelephotoPerfTarget& target, bool force = false) {
        const auto restore = [&]() {
            m_prospi_telephoto_perf_pending_valid = false;
            m_prospi_telephoto_perf_target_valid = false;
            m_prospi_telephoto_perf_line_mode = false;

            if (!m_prospi_telephoto_perf_override_applied) {
                m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
                return;
            }

            set_runtime_cvar_float(L"r.ViewDistanceScale", m_prospi_telephoto_perf_baseline_view_distance_scale);
            set_runtime_cvar_float(L"r.StaticMeshLODDistanceScale", m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale);
            set_runtime_cvar_int(L"r.SkeletalMeshLODBias", m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias);
            m_prospi_telephoto_perf_override_applied = false;
            m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
            spdlog::info(
                "[PROSPI_TELEPHOTO_PERF] active=false view_distance={:.2f} static_mesh_lod_scale={:.2f} skeletal_lod_bias={}",
                m_prospi_telephoto_perf_baseline_view_distance_scale,
                m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale,
                m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias
            );
        };

        if (!is_prospi_executable() || !m_match_game_fov_prospi_telephoto_perf_override->value()) {
            restore();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        constexpr auto debounce_window = std::chrono::milliseconds{750};

        if (m_prospi_telephoto_perf_override_applied != should_apply) {
            if (!m_prospi_telephoto_perf_pending_valid ||
                m_prospi_telephoto_perf_pending_state != should_apply)
            {
                m_prospi_telephoto_perf_pending_valid = true;
                m_prospi_telephoto_perf_pending_state = should_apply;
                m_prospi_telephoto_perf_pending_since = now;
            }

            if (!force && now - m_prospi_telephoto_perf_pending_since < debounce_window) {
                return;
            }
        } else {
            m_prospi_telephoto_perf_pending_valid = false;
        }

        if (!should_apply) {
            restore();
            return;
        }

        if (!m_prospi_telephoto_perf_baselines_valid) {
            m_prospi_telephoto_perf_baseline_view_distance_scale = get_runtime_cvar_float(L"r.ViewDistanceScale").value_or(1.0f);
            m_prospi_telephoto_perf_baseline_static_mesh_lod_distance_scale = get_runtime_cvar_float(L"r.StaticMeshLODDistanceScale").value_or(1.0f);
            m_prospi_telephoto_perf_baseline_skeletal_mesh_lod_bias = get_runtime_cvar_int(L"r.SkeletalMeshLODBias").value_or(0);
            m_prospi_telephoto_perf_baselines_valid = true;
        }

        const auto target_view_distance_scale = std::clamp(target.view_distance_scale, 0.10f, 2.0f);
        const auto target_static_mesh_lod_distance_scale = std::clamp(target.static_mesh_lod_distance_scale, 0.10f, 4.0f);
        const auto target_skeletal_mesh_lod_bias = std::clamp(target.skeletal_mesh_lod_bias, 0, 4);
        const auto target_changed =
            !m_prospi_telephoto_perf_target_valid ||
            std::abs(m_prospi_telephoto_perf_target_view_distance_scale - target_view_distance_scale) > 0.001f ||
            std::abs(m_prospi_telephoto_perf_target_static_mesh_lod_distance_scale - target_static_mesh_lod_distance_scale) > 0.001f ||
            m_prospi_telephoto_perf_target_skeletal_mesh_lod_bias != target_skeletal_mesh_lod_bias ||
            m_prospi_telephoto_perf_line_mode != target.line_mode;

        if (!m_prospi_telephoto_perf_override_applied || target_changed || force) {
            set_runtime_cvar_float(L"r.ViewDistanceScale", target_view_distance_scale);
            set_runtime_cvar_float(L"r.StaticMeshLODDistanceScale", target_static_mesh_lod_distance_scale);
            set_runtime_cvar_int(L"r.SkeletalMeshLODBias", target_skeletal_mesh_lod_bias);

            spdlog::info(
                "[PROSPI_TELEPHOTO_PERF] active=true mode={} view_distance={:.2f} static_mesh_lod_scale={:.2f} skeletal_lod_bias={}",
                target.line_mode ? "line" : "telephoto",
                target_view_distance_scale,
                target_static_mesh_lod_distance_scale,
                target_skeletal_mesh_lod_bias
            );

            m_prospi_telephoto_perf_target_valid = true;
            m_prospi_telephoto_perf_target_view_distance_scale = target_view_distance_scale;
            m_prospi_telephoto_perf_target_static_mesh_lod_distance_scale = target_static_mesh_lod_distance_scale;
            m_prospi_telephoto_perf_target_skeletal_mesh_lod_bias = target_skeletal_mesh_lod_bias;
            m_prospi_telephoto_perf_line_mode = target.line_mode;
        }

        m_prospi_telephoto_perf_override_applied = true;
        m_match_game_fov_prospi_telephoto_perf_active.store(true, std::memory_order_relaxed);
    };

    const auto reset_prospi_state = [&]() {
        update_prospi_telephoto_perf_override(false, {}, true);
        m_match_game_fov_prospi_preset.store((int32_t)ProSpiCameraPreset::None, std::memory_order_relaxed);
        m_match_game_fov_prospi_actual_min_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_applied.store(false, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_dolly_distance_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_multiplier_active.store(1.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_calibration_actual_min_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_tv_override_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_prospi_auto_dolly_distance_active.store(0.0f, std::memory_order_relaxed);
        m_match_game_fov_prospi_telephoto_perf_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_read_only_camera_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_would_write_game_camera.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
        m_match_game_fov_generic_camera_preset_applied.store(false, std::memory_order_relaxed);
        m_match_game_fov_generic_camera_tracking_active.store(false, std::memory_order_relaxed);

        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_current_camera_id.clear();
        m_prospi_sticky_preset_valid = false;
        m_prospi_sticky_preset = (int32_t)ProSpiCameraPreset::None;
        m_prospi_sticky_location = {};
        m_prospi_sticky_rotation = {};
        m_prospi_sticky_raw_fov = 0.0f;
        m_prospi_sticky_calibration_valid = false;
        m_prospi_sticky_camera_id.clear();
        m_prospi_line_telephoto_perf_hold_until = {};

        {
            std::scoped_lock generic_lock{m_generic_camera_preset_mtx};
            m_current_game_camera_id.clear();
            m_active_generic_camera_preset = {};
        }

        m_camera_cut_state = {};
    };

    if (!m_match_game_fov->value()) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto engine = sdk::UEngine::get();
    auto world = engine != nullptr ? engine->get_world() : nullptr;
    auto gameplay = sdk::UGameplayStatics::get();

    if (world == nullptr || gameplay == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto pc = gameplay->get_player_controller(world, 0);
    if (pc == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto pcm = pc->get_player_camera_manager();
    if (pcm == nullptr) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto fov = read_game_fov(pcm);
    if (!fov.has_value()) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    if (!std::isfinite(*fov) || *fov <= 0.01f || *fov >= 179.0f) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_raw.store(0.0f, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    const auto raw_fov = *fov;
    m_game_fov_raw.store(raw_fov, std::memory_order_relaxed);

    auto game_fov_for_matching = raw_fov;
    auto active_fov_multiplier = std::clamp(m_match_game_fov_multiplier->value(), 0.1f, 3.0f);
    auto active_dolly_distance = std::clamp(m_match_game_fov_dolly_distance->value(), 10.0f, 50000.0f);
    auto effective_fov = raw_fov * active_fov_multiplier;
    if (!std::isfinite(effective_fov)) {
        m_game_fov_valid.store(false, std::memory_order_relaxed);
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
        reset_prospi_state();
        return;
    }

    auto projection_min_fov = m_match_game_fov_min_enabled->value() ? m_match_game_fov_min->value() : 5.0f;
    const auto prospi_actual_min_fov = std::clamp(m_match_game_fov_prospi_actual_min->value(), 5.0f, 175.0f);
    auto prospi_preset = ProSpiCameraPreset::None;
    auto active_prospi_actual_min_fov = 0.0f;
    auto wrote_prospi_fov = false;
    auto wants_game_fov_write = false;
    auto deferred_game_fov_write = raw_fov;
    auto prospi_calibration_applied = false;
    auto prospi_tv_override_applied = false;
    auto generic_camera_preset_applied = false;
    auto read_only_camera_for_frame = m_match_game_fov_read_only_camera->value();
    const auto camera_cut_stabilizer_enabled = m_match_game_fov_camera_cut_stabilizer->value();
    const auto generic_camera_presets_tracking_enabled =
        m_match_game_fov_dolly->value() &&
        m_match_game_fov_generic_camera_presets->value();
    const auto generic_camera_presets_apply_enabled =
        generic_camera_presets_tracking_enabled &&
        m_match_game_fov_generic_camera_presets_auto_apply->value();
    const auto should_track_generic_camera = camera_cut_stabilizer_enabled || generic_camera_presets_tracking_enabled;
    const char* prospi_dolly_source = "Base";
    std::string prospi_camera_id{};

    const auto is_prospi = is_prospi_executable();
    const auto location = read_game_camera_location(pcm);
    const auto rotation = read_game_camera_rotation(pcm);
    GameCameraSample camera_sample{};
    if (should_track_generic_camera && location.has_value() && rotation.has_value()) {
        camera_sample.valid = true;
        camera_sample.player_camera_manager = reinterpret_cast<uintptr_t>(pcm);
        camera_sample.raw_fov = raw_fov;
        camera_sample.timestamp = std::chrono::steady_clock::now();
        camera_sample.location = *location;
        camera_sample.rotation = *rotation;
        camera_sample.camera_id = build_generic_camera_preset_id(*location, *rotation, raw_fov);
        {
            std::scoped_lock _{m_generic_camera_preset_mtx};
            m_current_game_camera_id = camera_sample.camera_id;
        }

        m_match_game_fov_generic_camera_tracking_active.store(true, std::memory_order_relaxed);
    } else if (m_match_game_fov_generic_camera_tracking_active.exchange(false, std::memory_order_relaxed)) {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_current_game_camera_id.clear();
    }

    if (location.has_value() && rotation.has_value()) {
        prospi_camera_id = build_camera_calibration_id(*location, *rotation);
        {
            std::scoped_lock _{m_prospi_camera_calibration_mtx};
            m_prospi_current_camera_id = prospi_camera_id;
        }

        if (m_match_game_fov_prospi_camera_calibration_auto->value()) {
            std::scoped_lock _{m_prospi_camera_calibration_mtx};
            if (const auto it = m_prospi_camera_calibrations.find(prospi_camera_id); it != m_prospi_camera_calibrations.end()) {
                const auto saved_min_fov = std::clamp(it->second.actual_min_fov, 5.0f, 175.0f);
                if (is_prospi) {
                    active_prospi_actual_min_fov = saved_min_fov;
                } else if (m_match_game_fov_min_enabled->value()) {
                    projection_min_fov = saved_min_fov;
                }

                active_fov_multiplier = std::clamp(it->second.projection_multiplier, 0.1f, 3.0f);
                active_dolly_distance = std::clamp(it->second.dolly_distance, 10.0f, 50000.0f);
                prospi_calibration_applied = true;
                prospi_dolly_source = "Calibration";
            }
        }
    } else {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_current_camera_id.clear();
    }

    if (is_prospi && location.has_value() && rotation.has_value()) {
        const auto classified_prospi_preset = classify_prospi_camera_preset(*location, *rotation, raw_fov);
        prospi_preset = classified_prospi_preset;

        if (m_prospi_sticky_preset_valid &&
            should_keep_prospi_sticky_preset(
                (ProSpiCameraPreset)m_prospi_sticky_preset,
                m_prospi_sticky_location,
                m_prospi_sticky_rotation,
                m_prospi_sticky_raw_fov,
                classified_prospi_preset,
                *location,
                *rotation,
                raw_fov)) {
            prospi_preset = (ProSpiCameraPreset)m_prospi_sticky_preset;
        }

        if (!prospi_calibration_applied && m_match_game_fov_dolly->value()) {
            switch (prospi_preset) {
            case ProSpiCameraPreset::OpeningAerialTelephoto:
                if (m_match_game_fov_prospi_opening_aerial_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_opening_aerial_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "OpeningAerialTelephoto";
                }
                break;
            case ProSpiCameraPreset::BehindPlateWideTelephoto:
                if (m_match_game_fov_prospi_behind_plate_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_behind_plate_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "BehindPlateWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateWaistHighReverse:
                if (m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateWaistHighReverse";
                }
                break;
            case ProSpiCameraPreset::TVBroadcast:
                if (m_match_game_fov_prospi_tv_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_tv_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_tv_override_applied = true;
                    prospi_dolly_source = "TVBroadcast";
                }
                break;
            case ProSpiCameraPreset::CenterFieldTelephoto:
                if (m_match_game_fov_prospi_center_field_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_center_field_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "CenterFieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::CenterFieldHighTelephoto:
                if (m_match_game_fov_prospi_center_field_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_center_field_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "CenterFieldHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
                if (m_match_game_fov_prospi_left_field_corner_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_left_field_corner_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "OffsetCenterFieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::DeepOutfieldTelephoto:
                if (m_match_game_fov_prospi_deep_outfield_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_deep_outfield_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "DeepOutfieldTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateSkyAerial:
                if (m_match_game_fov_prospi_home_plate_sky_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_sky_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateSkyAerial";
                }
                break;
            case ProSpiCameraPreset::PlateHighTelephoto:
                if (m_match_game_fov_prospi_plate_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_plate_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "PlateHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::HomePlateOverheadTelephoto:
                if (m_match_game_fov_prospi_home_plate_overhead_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_plate_overhead_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "HomePlateOverheadTelephoto";
                }
                break;
            case ProSpiCameraPreset::UpperDeckTelephoto:
                if (m_match_game_fov_prospi_upper_deck_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_upper_deck_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "UpperDeckTelephoto";
                }
                break;
            case ProSpiCameraPreset::UpperDeckHomeSkyTelephoto:
                if (m_match_game_fov_prospi_home_sky_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_home_sky_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "UpperDeckHomeSkyTelephoto";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseTelephoto:
                if (m_match_game_fov_prospi_third_base_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseTelephoto";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseRelayLow:
                if (m_match_game_fov_prospi_third_base_relay_low_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_relay_low_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseRelayLow";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseCornerLow:
                if (m_match_game_fov_prospi_third_base_sweep_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_sweep_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseCornerLow";
                }
                break;
            case ProSpiCameraPreset::ThirdBaseWideTelephoto:
                if (m_match_game_fov_prospi_third_base_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_third_base_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "ThirdBaseWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseTelephoto:
                if (m_match_game_fov_prospi_first_base_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseWideTelephoto:
                if (m_match_game_fov_prospi_first_base_wide_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_wide_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseWideTelephoto";
                }
                break;
            case ProSpiCameraPreset::FirstBaseCornerLow:
                if (m_match_game_fov_prospi_first_base_corner_low_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_first_base_corner_low_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "FirstBaseCornerLow";
                }
                break;
            case ProSpiCameraPreset::LowInfieldSideCloseUp:
                if (m_match_game_fov_prospi_low_plate_corner_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_low_plate_corner_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "LowInfieldSideCloseUp";
                }
                break;
            case ProSpiCameraPreset::BackstopHighTelephoto:
                if (m_match_game_fov_prospi_backstop_high_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_backstop_high_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "BackstopHighTelephoto";
                }
                break;
            case ProSpiCameraPreset::RightFieldCornerTelephoto:
                if (m_match_game_fov_prospi_right_field_corner_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_right_field_corner_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "RightFieldCornerTelephoto";
                }
                break;
            case ProSpiCameraPreset::RightCenterFieldTelephoto:
                if (m_match_game_fov_prospi_right_center_field_dolly_override->value()) {
                    active_dolly_distance = std::clamp(m_match_game_fov_prospi_right_center_field_dolly_distance->value(), 10.0f, 50000.0f);
                    prospi_dolly_source = "RightCenterFieldTelephoto";
                }
                break;
            default:
                break;
            }
        }

        if (is_specific_prospi_preset(prospi_preset)) {
            m_prospi_sticky_preset_valid = true;
            m_prospi_sticky_preset = (int32_t)prospi_preset;
            m_prospi_sticky_location = *location;
            m_prospi_sticky_rotation = *rotation;
            m_prospi_sticky_raw_fov = raw_fov;
            m_prospi_sticky_camera_id = prospi_camera_id;
        }
    }

    if (m_match_game_fov_dolly->value() &&
        m_match_game_fov_prospi_actual_clamp->value() &&
        is_prospi) {
        const auto resolve_prospi_actual_min_fov = [&](ProSpiCameraPreset preset) {
            switch (preset) {
            case ProSpiCameraPreset::CenterFieldTelephoto:
            case ProSpiCameraPreset::CenterFieldHighTelephoto:
            case ProSpiCameraPreset::OffsetCenterFieldTelephoto:
                return std::clamp(m_match_game_fov_prospi_center_field_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::UpperDeckTelephoto:
            case ProSpiCameraPreset::UpperDeckHomeSkyTelephoto:
                return std::clamp(m_match_game_fov_prospi_upper_deck_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::PlateHighTelephoto:
            case ProSpiCameraPreset::HomePlateOverheadTelephoto:
                return std::clamp(m_match_game_fov_prospi_plate_high_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::DeepOutfieldTelephoto:
                return std::clamp(m_match_game_fov_prospi_deep_outfield_actual_min->value(), 5.0f, 175.0f);
            case ProSpiCameraPreset::ThirdBaseTelephoto:
            case ProSpiCameraPreset::ThirdBaseRelayLow:
            case ProSpiCameraPreset::ThirdBaseWideTelephoto:
            case ProSpiCameraPreset::FirstBaseTelephoto:
            case ProSpiCameraPreset::FirstBaseWideTelephoto:
            case ProSpiCameraPreset::GenericTelephoto:
            case ProSpiCameraPreset::None:
            default:
                return prospi_actual_min_fov;
            }
        };

        if (!prospi_calibration_applied) {
            active_prospi_actual_min_fov = resolve_prospi_actual_min_fov(prospi_preset);
        }

        const auto generic_trigger_min_fov = prospi_calibration_applied ? active_prospi_actual_min_fov : prospi_actual_min_fov;

        if (prospi_preset != ProSpiCameraPreset::None || raw_fov < generic_trigger_min_fov) {
            if (prospi_preset == ProSpiCameraPreset::None) {
                prospi_preset = ProSpiCameraPreset::GenericTelephoto;
                if (!prospi_calibration_applied) {
                    active_prospi_actual_min_fov = resolve_prospi_actual_min_fov(prospi_preset);
                }
            }

            game_fov_for_matching = std::clamp(raw_fov, active_prospi_actual_min_fov, 175.0f);
            if (std::abs(game_fov_for_matching - raw_fov) > 0.01f) {
                wants_game_fov_write = true;
                deferred_game_fov_write = game_fov_for_matching;
            }
        }
    }

    if (generic_camera_presets_apply_enabled && camera_sample.valid) {
        std::optional<GenericCameraPreset> preset{};
        {
            std::scoped_lock _{m_generic_camera_preset_mtx};
            if (const auto it = m_generic_camera_presets.find(camera_sample.camera_id); it != m_generic_camera_presets.end()) {
                preset = it->second;
                m_active_generic_camera_preset = it->second;
            } else {
                m_active_generic_camera_preset = {};
            }
        }

        if (preset.has_value()) {
            generic_camera_preset_applied = true;
            active_fov_multiplier = std::clamp(preset->projection_multiplier, 0.1f, 3.0f);
            active_dolly_distance = std::clamp(preset->dolly_distance, 10.0f, 50000.0f);
            projection_min_fov = std::max(projection_min_fov, std::clamp(preset->min_fov, 5.0f, 175.0f));
            game_fov_for_matching = std::clamp(game_fov_for_matching, projection_min_fov, 175.0f);
            read_only_camera_for_frame = read_only_camera_for_frame || preset->read_only_camera;
        }
    } else {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_active_generic_camera_preset = {};
    }

    effective_fov = game_fov_for_matching * active_fov_multiplier;
    effective_fov = std::clamp(effective_fov, projection_min_fov, 175.0f);

    auto telephoto_perf_target = ProSpiTelephotoPerfTarget{
        std::clamp(m_match_game_fov_prospi_telephoto_perf_view_distance_scale->value(), 0.10f, 2.0f),
        std::clamp(m_match_game_fov_prospi_telephoto_perf_static_mesh_lod_distance_scale->value(), 0.10f, 4.0f),
        std::clamp((int)std::lround(m_match_game_fov_prospi_telephoto_perf_skeletal_mesh_lod_bias->value()), 0, 4),
        false
    };

    const auto now = std::chrono::steady_clock::now();
    const auto line_telephoto_perf_candidate =
        is_prospi &&
        (is_prospi_line_telephoto_perf_preset(prospi_preset) ||
         (prospi_preset == ProSpiCameraPreset::GenericTelephoto &&
          location.has_value() &&
          rotation.has_value() &&
          is_prospi_line_telephoto_perf_location(*location, *rotation, raw_fov)));

    if (line_telephoto_perf_candidate) {
        m_prospi_line_telephoto_perf_hold_until = now + std::chrono::seconds{12};
    }

    const auto line_telephoto_perf_active =
        m_prospi_line_telephoto_perf_hold_until.time_since_epoch().count() != 0 &&
        now < m_prospi_line_telephoto_perf_hold_until;

    if (line_telephoto_perf_active) {
        telephoto_perf_target.view_distance_scale = std::min(telephoto_perf_target.view_distance_scale, 0.35f);
        telephoto_perf_target.static_mesh_lod_distance_scale = std::max(telephoto_perf_target.static_mesh_lod_distance_scale, 3.0f);
        telephoto_perf_target.skeletal_mesh_lod_bias = std::max(telephoto_perf_target.skeletal_mesh_lod_bias, 2);
        telephoto_perf_target.line_mode = true;
    }

    bool camera_cut_stabilizer_blocked_write = false;
    if (!camera_cut_stabilizer_enabled) {
        if (m_camera_cut_state.stabilizing) {
            spdlog::info("[CAMERA_STABILIZER] active=false reason=disabled");
        }

        m_camera_cut_state = {};
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
    } else if (camera_sample.valid) {
        auto output = GameCameraProjectionState{
            .valid = true,
            .game_fov_for_matching = game_fov_for_matching,
            .effective_fov = effective_fov,
            .active_dolly_distance = active_dolly_distance,
            .active_fov_multiplier = active_fov_multiplier
        };

        const auto now = camera_sample.timestamp;
        const auto duration_ms = std::clamp(m_match_game_fov_camera_cut_stabilizer_duration_ms->value(), 100.0f, 1500.0f);
        const auto fov_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_fov_delta->value(), 1.0f, 45.0f);
        const auto rotation_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_rotation_delta->value(), 1.0f, 90.0f);
        const auto location_threshold = std::clamp(m_match_game_fov_camera_cut_stabilizer_location_delta->value(), 25.0f, 10000.0f);

        bool detected_cut = false;
        float location_delta = 0.0f;
        float rotation_delta = 0.0f;
        float fov_delta = 0.0f;
        bool camera_id_changed = false;
        bool pcm_changed = false;

        if (m_camera_cut_state.has_previous_sample) {
            const auto& previous = m_camera_cut_state.previous_sample;
            location_delta = glm::distance(camera_sample.location, previous.location);
            const auto pitch_delta = normalize_angle_delta(camera_sample.rotation.x, previous.rotation.x);
            const auto yaw_delta = normalize_angle_delta(camera_sample.rotation.y, previous.rotation.y);
            const auto roll_delta = normalize_angle_delta(camera_sample.rotation.z, previous.rotation.z);
            rotation_delta = (std::max)(pitch_delta, (std::max)(yaw_delta, roll_delta));
            fov_delta = std::abs(camera_sample.raw_fov - previous.raw_fov);
            camera_id_changed = camera_sample.camera_id != previous.camera_id;
            pcm_changed = camera_sample.player_camera_manager != previous.player_camera_manager;

            detected_cut =
                camera_id_changed ||
                pcm_changed ||
                fov_delta >= fov_threshold ||
                rotation_delta >= rotation_threshold ||
                location_delta >= location_threshold;
        }

        if (detected_cut && m_camera_cut_state.has_last_output) {
            m_camera_cut_state.stabilizing = true;
            m_camera_cut_state.cut_time = now;
            m_camera_cut_state.stabilize_until = now + std::chrono::milliseconds((int64_t)std::lround(duration_ms));
            m_camera_cut_state.blend_from = m_camera_cut_state.last_output;
            m_camera_cut_state.blend_to = output;
            m_camera_cut_state.last_cut_from = m_camera_cut_state.previous_sample;
            m_camera_cut_state.last_cut_to = camera_sample;

            spdlog::info(
                "[CAMERA_CUT] from={} to={} id_changed={} pcm_changed={} loc_delta={:.1f} rot_delta={:.1f} fov_delta={:.1f} stabilize_ms={:.0f}",
                m_camera_cut_state.last_cut_from.camera_id.empty() ? "None" : m_camera_cut_state.last_cut_from.camera_id,
                camera_sample.camera_id.empty() ? "None" : camera_sample.camera_id,
                camera_id_changed,
                pcm_changed,
                location_delta,
                rotation_delta,
                fov_delta,
                duration_ms);
        }

        if (m_camera_cut_state.stabilizing) {
            m_camera_cut_state.blend_to = output;

            if (now < m_camera_cut_state.stabilize_until) {
                const auto elapsed_ms = (float)std::chrono::duration_cast<std::chrono::milliseconds>(now - m_camera_cut_state.cut_time).count();
                const auto freeze_ms = duration_ms * 0.4f;
                const auto blend_ms = (std::max)(1.0f, duration_ms - freeze_ms);
                const auto t = elapsed_ms <= freeze_ms ? 0.0f : smoothstep01((elapsed_ms - freeze_ms) / blend_ms);
                const auto& from = m_camera_cut_state.blend_from;
                const auto& to = m_camera_cut_state.blend_to;

                game_fov_for_matching = lerp_float(from.game_fov_for_matching, to.game_fov_for_matching, t);
                effective_fov = std::clamp(lerp_float(from.effective_fov, to.effective_fov, t), projection_min_fov, 175.0f);
                active_dolly_distance = lerp_float(from.active_dolly_distance, to.active_dolly_distance, t);
                active_fov_multiplier = lerp_float(from.active_fov_multiplier, to.active_fov_multiplier, t);
                camera_cut_stabilizer_blocked_write = true;

                const auto remaining_ms =
                    (int32_t)std::chrono::duration_cast<std::chrono::milliseconds>(m_camera_cut_state.stabilize_until - now).count();
                m_match_game_fov_camera_cut_stabilizer_active.store(true, std::memory_order_relaxed);
                m_match_game_fov_camera_cut_stabilizer_remaining_ms.store((std::max)(0, remaining_ms), std::memory_order_relaxed);
            } else {
                m_camera_cut_state.stabilizing = false;
                m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
                m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
                spdlog::info("[CAMERA_STABILIZER] active=false reason=complete");
            }
        } else {
            m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
            m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
        }

        m_camera_cut_state.previous_sample = camera_sample;
        m_camera_cut_state.has_previous_sample = true;
        m_camera_cut_state.last_output = GameCameraProjectionState{
            .valid = true,
            .game_fov_for_matching = game_fov_for_matching,
            .effective_fov = effective_fov,
            .active_dolly_distance = active_dolly_distance,
            .active_fov_multiplier = active_fov_multiplier
        };
        m_camera_cut_state.has_last_output = true;
    } else {
        m_camera_cut_state = {};
        m_match_game_fov_camera_cut_stabilizer_active.store(false, std::memory_order_relaxed);
        m_match_game_fov_camera_cut_stabilizer_remaining_ms.store(0, std::memory_order_relaxed);
    }

    const auto block_game_fov_write = read_only_camera_for_frame || camera_cut_stabilizer_blocked_write;
    if (wants_game_fov_write) {
        if (block_game_fov_write) {
            m_match_game_fov_would_write_game_camera.store(true, std::memory_order_relaxed);
        } else {
            wrote_prospi_fov = write_game_fov(pcm, deferred_game_fov_write);
            m_match_game_fov_would_write_game_camera.store(!wrote_prospi_fov, std::memory_order_relaxed);
        }
    } else {
        m_match_game_fov_would_write_game_camera.store(false, std::memory_order_relaxed);
    }

    m_match_game_fov_read_only_camera_active.store(block_game_fov_write, std::memory_order_relaxed);
    m_match_game_fov_generic_camera_preset_applied.store(generic_camera_preset_applied, std::memory_order_relaxed);

    const auto telephoto_perf_trigger_fov = std::clamp(m_match_game_fov_prospi_telephoto_perf_trigger_fov->value(), 10.0f, 40.0f);
    const auto telephoto_perf_should_apply =
        is_prospi &&
        m_match_game_fov_prospi_telephoto_perf_override->value() &&
        (line_telephoto_perf_active ||
         (!is_prospi_nontelephoto_preset(prospi_preset) &&
          (raw_fov <= telephoto_perf_trigger_fov || game_fov_for_matching <= telephoto_perf_trigger_fov)));
    update_prospi_telephoto_perf_override(telephoto_perf_should_apply, telephoto_perf_target);

    m_match_game_fov_prospi_preset.store((int32_t)prospi_preset, std::memory_order_relaxed);
    m_match_game_fov_prospi_actual_min_active.store(active_prospi_actual_min_fov, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_applied.store(prospi_calibration_applied, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_dolly_distance_active.store(active_dolly_distance, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_multiplier_active.store(active_fov_multiplier, std::memory_order_relaxed);
    m_match_game_fov_prospi_calibration_actual_min_active.store(prospi_calibration_applied ? active_prospi_actual_min_fov : 0.0f, std::memory_order_relaxed);
    m_match_game_fov_prospi_tv_override_active.store(prospi_tv_override_applied, std::memory_order_relaxed);
    m_match_game_fov_prospi_auto_dolly_distance_active.store(prospi_dolly_source == std::string_view{"Base"} ? 0.0f : active_dolly_distance, std::memory_order_relaxed);
    m_game_fov.store(effective_fov, std::memory_order_relaxed);
    m_game_fov_valid.store(true, std::memory_order_relaxed);

    if (is_prospi_executable() && m_match_game_fov_prospi_actual_clamp->value()) {
        static auto last_logged_preset = ProSpiCameraPreset::None;
        static auto last_logged_raw_fov = 0.0f;
        static auto last_logged_min_fov = 0.0f;
        static auto last_logged_written_fov = 0.0f;
        static auto last_logged_effective_fov = 0.0f;
        static auto last_logged_write_state = false;
        static auto last_logged_calibration_state = false;
        static auto last_logged_tv_override_state = false;
        static auto last_logged_telephoto_perf_state = false;
        static auto last_logged_multiplier = 1.0f;
        static auto last_logged_dolly_distance = 0.0f;
        static std::string last_logged_dolly_source{"Base"};
        static std::string last_logged_camera_id{};

        if (prospi_preset != last_logged_preset ||
            wrote_prospi_fov != last_logged_write_state ||
            prospi_calibration_applied != last_logged_calibration_state ||
            prospi_tv_override_applied != last_logged_tv_override_state ||
            telephoto_perf_should_apply != last_logged_telephoto_perf_state ||
            prospi_dolly_source != last_logged_dolly_source ||
            prospi_camera_id != last_logged_camera_id ||
            std::abs(raw_fov - last_logged_raw_fov) > 0.25f ||
            std::abs(active_prospi_actual_min_fov - last_logged_min_fov) > 0.01f ||
            std::abs(active_fov_multiplier - last_logged_multiplier) > 0.01f ||
            std::abs(active_dolly_distance - last_logged_dolly_distance) > 0.25f ||
            std::abs(game_fov_for_matching - last_logged_written_fov) > 0.25f ||
            std::abs(effective_fov - last_logged_effective_fov) > 0.25f) {
            spdlog::info(
                "[PROSPI_FOV] preset={} camera={} calibrated={} tv_override={} telephoto_perf={} dolly_source={} raw={:.2f} min={:.2f} mult={:.2f} dolly={:.2f} written={:.2f} effective={:.2f} wrote={}",
                get_prospi_camera_preset_name(prospi_preset),
                prospi_camera_id.empty() ? "None" : prospi_camera_id,
                prospi_calibration_applied,
                prospi_tv_override_applied,
                telephoto_perf_should_apply,
                prospi_dolly_source,
                raw_fov,
                active_prospi_actual_min_fov,
                active_fov_multiplier,
                active_dolly_distance,
                game_fov_for_matching,
                effective_fov,
                wrote_prospi_fov
            );

            last_logged_preset = prospi_preset;
            last_logged_raw_fov = raw_fov;
            last_logged_min_fov = active_prospi_actual_min_fov;
            last_logged_multiplier = active_fov_multiplier;
            last_logged_dolly_distance = active_dolly_distance;
            last_logged_written_fov = game_fov_for_matching;
            last_logged_effective_fov = effective_fov;
            last_logged_write_state = wrote_prospi_fov;
            last_logged_calibration_state = prospi_calibration_applied;
            last_logged_tv_override_state = prospi_tv_override_applied;
            last_logged_telephoto_perf_state = telephoto_perf_should_apply;
            last_logged_dolly_source = prospi_dolly_source;
            last_logged_camera_id = prospi_camera_id;
        }
    }

    if (m_match_game_fov_dolly->value()) {
        auto base_fov = read_default_fov(pcm).value_or(m_game_fov_base.load(std::memory_order_relaxed));
        if (!std::isfinite(base_fov) || base_fov <= 1.0f || base_fov >= 179.0f) {
            base_fov = game_fov_for_matching;
        }

        m_game_fov_base.store(base_fov, std::memory_order_relaxed);
        base_fov = std::clamp(base_fov, 5.0f, 175.0f);

        const auto current_half = glm::radians(effective_fov) * 0.5f;
        const auto base_half = glm::radians(base_fov) * 0.5f;
        const auto base_tan = std::tan(base_half);
        const auto current_tan = std::tan(current_half);

        if (base_tan <= 0.0f || current_tan <= 0.0f) {
            m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
            return;
        }

        const auto scale = current_tan / base_tan;
        const auto focus_distance = active_dolly_distance;
        auto dolly_offset = focus_distance * (1.0f - scale);
        const auto max_offset = focus_distance * 2.0f;
        dolly_offset = std::clamp(dolly_offset, -max_offset, max_offset);

        if (!std::isfinite(dolly_offset)) {
            m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
            return;
        }

        m_game_fov_dolly_offset.store(dolly_offset, std::memory_order_relaxed);
    } else {
        m_game_fov_dolly_offset.store(0.0f, std::memory_order_relaxed);
    }
}

float VR::get_game_fov() const {
    return m_game_fov.load(std::memory_order_relaxed);
}

float VR::get_game_fov_scale(float base_half_fov) const {
    if (!m_game_fov_valid.load(std::memory_order_relaxed)) {
        return 1.0f;
    }

    if (m_match_game_fov_dolly->value()) {
        return 1.0f;
    }

    auto game_fov = get_game_fov();

    if (!std::isfinite(game_fov)) {
        return 1.0f;
    }

    game_fov = std::clamp(game_fov, 5.0f, 175.0f);

    const auto desired_half = glm::radians(game_fov) * 0.5f;
    const auto base_tan = std::tan(base_half_fov);
    const auto desired_tan = std::tan(desired_half);

    if (base_tan <= 0.0f || desired_tan <= 0.0f) {
        return 1.0f;
    }

    const auto scale = base_tan / desired_tan;
    if (!std::isfinite(scale) || scale <= 0.01f || scale >= 100.0f) {
        return 1.0f;
    }

    return scale;
}

float VR::get_game_fov_dolly_offset() const {
    return m_game_fov_dolly_offset.load(std::memory_order_relaxed);
}

void VR::on_pre_calculate_stereo_view_offset(void* stereo_device, const int32_t view_index, Rotator<float>* view_rotation, 
                                             const float world_to_meters, Vector3f* view_location, bool is_double)
{
    if (!is_hmd_active()) {
        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.rotation_wants_freeze = false;
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const auto delta = std::chrono::duration<float, std::chrono::seconds::period>(now - m_last_lerp_update).count();

    Rotator<double>* view_rotation_double = (Rotator<double>*)view_rotation;
    Vector3d* view_location_double = (Vector3d*)view_location;

    glm::vec3 target_rotation = is_double ? glm::vec3{*(glm::vec<3, double>*)view_rotation_double} : *(glm::vec<3, float>*)view_rotation;

    const auto should_lerp_pitch = m_lerp_camera_pitch->value();
    const auto should_lerp_yaw = m_lerp_camera_yaw->value();
    const auto should_lerp_roll = m_lerp_camera_roll->value();

    auto lerp_angle = [](auto a, auto b, auto t) {
        const auto diff = b - a;
        if constexpr (std::is_same_v<decltype(a), double>) {
            if (diff > 180.0) {
                b -= 360.0;
            } else if (diff < -180.0) {
                b += 360.0;
            }
        } else {
            if (diff > 180.0f) {
                b -= 360.0f;
            } else if (diff < -180.0f) {
                b += 360.0f;
            }
        }

        return glm::lerp(a, b, t);
    };

    const auto lerp_t = m_lerp_camera_speed->value() * delta;

    if (should_lerp_pitch) {
        if (is_double) {
            view_rotation_double->pitch = lerp_angle((double)m_camera_lerp.last_rotation.x, (double)target_rotation.x, (double)lerp_t);
        } else {
            view_rotation->pitch = lerp_angle(m_camera_lerp.last_rotation.x, target_rotation.x, lerp_t);
        }
    }

    if (should_lerp_yaw) {
        if (is_double) {
            view_rotation_double->yaw = lerp_angle((double)m_camera_lerp.last_rotation.y, (double)target_rotation.y, (double)lerp_t);
        } else {
            view_rotation->yaw = lerp_angle(m_camera_lerp.last_rotation.y, target_rotation.y, lerp_t);
        }
    }

    if (should_lerp_roll) {
        if (is_double) {
            view_rotation_double->roll = lerp_angle((double)m_camera_lerp.last_rotation.z, (double)target_rotation.z, (double)lerp_t);
        } else {
            view_rotation->roll = lerp_angle(m_camera_lerp.last_rotation.z, target_rotation.z, lerp_t);
        }
    }

    if (is_double) {
        m_camera_lerp.last_rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
    } else {
        m_camera_lerp.last_rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
    }

    m_last_lerp_update = std::chrono::high_resolution_clock::now();

    if (m_camera_freeze.position_wants_freeze) {
        if (is_double) {
            m_camera_freeze.position = glm::vec3{ (float)view_location_double->x, (float)view_location_double->y, (float)view_location_double->z };
        } else {
            m_camera_freeze.position = glm::vec3{ view_location->x, view_location->y, view_location->z };
        }

        m_camera_freeze.position_wants_freeze = false;
        m_camera_freeze.position_frozen = true;
    }

    if (m_camera_freeze.rotation_wants_freeze) {
        if (is_double) {
            m_camera_freeze.rotation = glm::vec3{ (float)view_rotation_double->pitch, (float)view_rotation_double->yaw, (float)view_rotation_double->roll };
        } else {
            m_camera_freeze.rotation = glm::vec3{ view_rotation->pitch, view_rotation->yaw, view_rotation->roll };
        }

        m_camera_freeze.rotation_wants_freeze = false;
        m_camera_freeze.rotation_frozen = true;
    }

    if (m_camera_freeze.position_frozen) {
        if (is_double) {
            view_location_double->x = m_camera_freeze.position.x;
            view_location_double->y = m_camera_freeze.position.y;
            view_location_double->z = m_camera_freeze.position.z;
        } else {
            view_location->x = m_camera_freeze.position.x;
            view_location->y = m_camera_freeze.position.y;
            view_location->z = m_camera_freeze.position.z;
        }
    }

    if (m_camera_freeze.rotation_frozen) {
        if (is_double) {
            view_rotation_double->pitch = m_camera_freeze.rotation.x;
            view_rotation_double->yaw = m_camera_freeze.rotation.y;
            view_rotation_double->roll = m_camera_freeze.rotation.z;
        } else {
            view_rotation->pitch = m_camera_freeze.rotation.x;
            view_rotation->yaw = m_camera_freeze.rotation.y;
            view_rotation->roll = m_camera_freeze.rotation.z;
        }
    }
}

void VR::on_pre_viewport_client_draw(void* viewport_client, void* viewport, void* canvas){
    ZoneScopedN(__FUNCTION__);

    if (m_custom_z_near_enabled->value()) {
        SPDLOG_INFO_ONCE("Attempting to set custom z near");
        sdk::globals::get_near_clipping_plane() = m_custom_z_near->value();
    }
}

void VR::update_hmd_state(bool from_view_extensions, uint32_t frame_count) {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_reinitialize_mtx};

    auto runtime = get_runtime();
    if (m_uncap_framerate->value()) {
        sdk::set_cvar_data_float(L"Engine", L"t.MaxFPS", 500.0f);
    }

    // Allows games running in HDR mode to not have a black UI overlay
    if (m_disable_hdr_compositing->value()) {
        sdk::set_cvar_data_int(L"SlateRHIRenderer", L"r.HDR.UI.CompositeMode", 0);
    }

    if (m_disable_blur_widgets->value()) {
        if (auto val = sdk::get_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets"); val && *val != 0) {
            sdk::set_cvar_int(L"Slate", L"Slate.AllowBackgroundBlurWidgets", 0);
        }
    }

    if (!is_using_afr()) {
        const auto is_hzbo_frozen_by_cvm = m_cvar_manager != nullptr && m_cvar_manager->is_hzbo_frozen_and_enabled();

        // Forcefully disable r.HZBOcclusion, it doesn't work with native stereo mode (sometimes)
        // Except when the user sets it to 1 with the CVar Manager, we need to respect that
        if (m_disable_hzbocclusion->value() && !is_hzbo_frozen_by_cvm) {
            const auto r_hzb_occlusion_value = sdk::get_cvar_int(L"Renderer", L"r.HZBOcclusion");

            // Only set it once, otherwise we'll be spamming a Set call every frame
            if (r_hzb_occlusion_value && *r_hzb_occlusion_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.HZBOcclusion", 0);
            }
        }

        if (m_disable_instance_culling->value()) {
            const auto r_instance_culling_value = sdk::get_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull");

            if (r_instance_culling_value && *r_instance_culling_value != 0) {
                sdk::set_cvar_int(L"Renderer", L"r.InstanceCulling.OcclusionCull", 0);
            }
        }
    }

    if (frame_count != 0 && is_using_afr() && frame_count % 2 == 0) {
        if (runtime->is_openxr()) {
            std::scoped_lock __{ m_openxr->sync_assignment_mtx };

            const auto last_frame = (frame_count - 1) % runtimes::OpenXR::QUEUE_SIZE;
            const auto now_frame = frame_count % runtimes::OpenXR::QUEUE_SIZE;
            m_openxr->pipeline_states[now_frame] = m_openxr->pipeline_states[last_frame];
            m_openxr->pipeline_states[now_frame].frame_count = now_frame;
        } else {
            const auto last_frame = (frame_count - 1) % m_openvr->pose_queue.size();
            const auto now_frame = frame_count % m_openvr->pose_queue.size();
            m_openvr->pose_queue[now_frame] = m_openvr->pose_queue[last_frame];
        }

        // Forcefully disable motion blur because it freaks out with AFR
        sdk::set_cvar_data_int(L"Engine", L"r.DefaultFeature.MotionBlur", 0);
        return;
    }
    
    runtime->update_poses(from_view_extensions, frame_count);

    // Update the poses used for the game
    // If we used the data directly from the WaitGetPoses call, we would have to lock a different mutex and wait a long time
    // This is because the WaitGetPoses call is blocking, and we don't want to block any game logic
    if (runtime->wants_reset_origin && runtime->ready() && runtime->got_first_valid_poses) {
        std::unique_lock _{ runtime->pose_mtx };
        set_rotation_offset(glm::identity<glm::quat>());
        m_standing_origin = get_position_unsafe(vr::k_unTrackedDeviceIndex_Hmd);

        runtime->wants_reset_origin = false;
    }

    runtime->update_matrices(m_nearz, m_farz);

    runtime->got_first_poses = true;
}

void VR::update_action_states() {
    ZoneScopedN(__FUNCTION__);

    std::scoped_lock _{m_actions_mtx};

    auto runtime = get_runtime();

    if (runtime == nullptr || runtime->wants_reinitialize) {
        return;
    }

    static bool once = true;

    if (once) {
        spdlog::info("VR: Updating action states");
        once = false;
    }


    if (runtime->is_openvr()) {
        const auto start_time = std::chrono::high_resolution_clock::now();

        auto error = vr::VRInput()->UpdateActionState(&m_active_action_set, sizeof(m_active_action_set), 1);

        if (error != vr::VRInputError_None) {
            spdlog::error("VRInput failed to update action state: {}", (uint32_t)error);
        }

        const auto end_time = std::chrono::high_resolution_clock::now();
        const auto time_delta = end_time - start_time;

        m_last_input_delay = time_delta;
        m_avg_input_delay = (m_avg_input_delay + time_delta) / 2;

        if ((end_time - start_time) >= std::chrono::milliseconds(30)) {
            spdlog::warn("VRInput update action state took too long: {}ms", std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

            //reinitialize_openvr();
            runtime->wants_reinitialize = true;
        }   
    } else {
        get_runtime()->update_input();
    }

    bool actively_using_controller = false;

    if (is_any_action_down()) {
        m_last_controller_update = std::chrono::steady_clock::now();
        actively_using_controller = true;
    }

    const auto last_xinput_update_is_late = std::chrono::steady_clock::now() - m_last_xinput_update >= std::chrono::seconds(2);
    const auto should_be_spoofing = (actively_using_controller || get_runtime()->handle_pause);

    if (m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        m_spoofed_gamepad_connection = false;
    }

    if (!m_spoofed_gamepad_connection && last_xinput_update_is_late && should_be_spoofing) {
        spdlog::info("[VR] Attempting to spoof gamepad connection");
        g_framework->post_message(WM_DEVICECHANGE, 0, 0);
        g_framework->activate_window();

        m_last_xinput_spoof_sent = std::chrono::steady_clock::now();
    }

    /*if (m_recenter_view_key->is_key_down_once()) {
        recenter_view();
    }

    if (m_set_standing_key->is_key_down_once()) {
        set_standing_origin(get_position(0));
    }*/

    static bool once2 = true;

    if (once2) {
        spdlog::info("VR: Updated action states");
        once2 = false;
    }

    update_dpad_gestures();
}

void VR::update_dpad_gestures() {
    if (!is_hmd_active()) {
        return;
    }

    const auto dpad_method = get_dpad_method();
    if (dpad_method != DPadMethod::GESTURE_HEAD && dpad_method != DPadMethod::GESTURE_HEAD_RIGHT) {
        return;
    }

    const auto wanted_index = dpad_method == DPadMethod::GESTURE_HEAD ? get_left_controller_index() : get_right_controller_index();

    const auto controller_pos = glm::vec3{get_position(wanted_index)};
    const auto hmd_transform = get_hmd_transform(m_frame_count);

    // Check if controller is near HMD
    const auto dist = glm::length(controller_pos - glm::vec3{hmd_transform[3]});

    if (dist > 0.2f) {
        return;
    }

    const auto dir_to_left = glm::normalize(controller_pos - glm::vec3{hmd_transform[3]});
    const auto hmd_dir = glm::quat{glm::extractMatrixRotation(hmd_transform)} * glm::vec3{0.0f, 0.0f, 1.0f};

    const auto angle = glm::acos(glm::dot(dir_to_left, hmd_dir));

    constexpr float threshold = glm::radians(120.0f);

    if (angle > threshold) {
        return;
    }

    // Make sure the angle is to the left/right of the HMD
    if (dpad_method == DPadMethod::GESTURE_HEAD_RIGHT) {
        if (glm::cross(dir_to_left, hmd_dir).y > 0.0f) {
            return;
        }
    } else if (glm::cross(dir_to_left, hmd_dir).y < 0.0f) {
        return;
    }

    // Send a vibration pulse to the controller
    const auto chosen_joystick = dpad_method == DPadMethod::GESTURE_HEAD ? m_left_joystick : m_right_joystick;
    trigger_haptic_vibration(0.0f, 0.1f, 1.0f, 5.0f, chosen_joystick);

    std::scoped_lock _{m_dpad_gesture_state.mtx};

    const auto left_joystick_axis = get_joystick_axis(chosen_joystick);

    if (left_joystick_axis.x < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::LEFT;
    } else if (left_joystick_axis.x > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::RIGHT;
    } 
    
    if (left_joystick_axis.y < -0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::DOWN;
    } else if (left_joystick_axis.y > 0.5f) {
        m_dpad_gesture_state.direction |= DPadGestureState::Direction::UP;
    }
}

void VR::on_config_load(const utility::Config& cfg, bool set_defaults) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_load(cfg, set_defaults);
    }

    if (get_runtime() != nullptr && get_runtime()->loaded) {
        get_runtime()->on_config_load(cfg, set_defaults);

        // Run the rest of OpenXR initialization code here that depends on config values
        if (m_first_config_load) {
            m_first_config_load = false; // because the frontend can request config reloads

            if (get_runtime()->is_openxr()) {
                spdlog::info("[VR] Finishing up OpenXR initialization");
                initialize_openxr_swapchains();
            }
        }
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_load(cfg, set_defaults);
    }

    m_overlay_component.on_config_load(cfg, set_defaults);

    if (m_cvar_manager != nullptr) {
        m_cvar_manager->on_config_load(cfg, set_defaults);   
    }

    // Load camera offsets
    load_cameras();
    load_prospi_camera_calibrations();
    load_generic_camera_presets();
}

void VR::on_config_save(utility::Config& cfg) {
    ZoneScopedN(__FUNCTION__);

    for (IModValue& option : m_options) {
        option.config_save(cfg);
    }

    if (m_fake_stereo_hook != nullptr) {
        m_fake_stereo_hook->on_config_save(cfg);
    }

    if (get_runtime()->loaded) {
        get_runtime()->on_config_save(cfg);
    }

    m_overlay_component.on_config_save(cfg);

    // Save camera offsets
    save_cameras();
    save_prospi_camera_calibrations();
}

void VR::load_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    if (std::filesystem::exists(cameras_txt)) {
        spdlog::info("[VR] Loading camera offsets from {}", cameras_txt.string());

        utility::Config cfg{cameras_txt.string()};

        for (auto i = 0; i < m_camera_datas.size(); i++) {
            auto& data = m_camera_datas[i];

            if (auto offs = cfg.get<float>(std::format("camera_right_offset{}", i))) {
                data.offset.x = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_up_offset{}", i))) {
                data.offset.y = *offs;
            }

            if (auto offs = cfg.get<float>(std::format("camera_forward_offset{}", i))) {
                data.offset.z = *offs;
            }

            if (auto scale = cfg.get<float>(std::format("world_scale{}", i))) {
                data.world_scale = *scale;
            }

            if (auto decoupled_pitch = cfg.get<bool>(std::format("decoupled_pitch{}", i))) {
                data.decoupled_pitch = *decoupled_pitch;
            }

            if (auto decoupled_pitch_ui_adjust = cfg.get<bool>(std::format("decoupled_pitch_ui_adjust{}", i))) {
                data.decoupled_pitch_ui_adjust = *decoupled_pitch_ui_adjust;
            }
        }
    }
} catch(...) {
    spdlog::error("[VR] Failed to load camera offsets");
}

void VR::load_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    const auto& data = m_camera_datas[index];

    m_camera_right_offset->value() = data.offset.x;
    m_camera_up_offset->value() = data.offset.y;
    m_camera_forward_offset->value() = data.offset.z;
    m_world_scale->value() = data.world_scale;
    m_decoupled_pitch->value() = data.decoupled_pitch;
    m_decoupled_pitch_ui_adjust->value() = data.decoupled_pitch_ui_adjust;
}

void VR::save_camera(int index) {
    ZoneScopedN(__FUNCTION__);

    if (index < 0 || index >= m_camera_datas.size()) {
        return;
    }

    auto& data = m_camera_datas[index];

    data.offset = {
        m_camera_right_offset->value(),
        m_camera_up_offset->value(),
        m_camera_forward_offset->value()
    };

    data.world_scale = m_world_scale->value();
    data.decoupled_pitch = m_decoupled_pitch->value();
    data.decoupled_pitch_ui_adjust = m_decoupled_pitch_ui_adjust->value();

    save_cameras();
}

void VR::save_cameras() try {
    ZoneScopedN(__FUNCTION__);

    const auto cameras_txt = Framework::get_persistent_dir("cameras.txt");

    spdlog::info("[VR] Saving camera offsets to {}", cameras_txt.string());

    utility::Config cfg{cameras_txt.string()};

    for (auto i = 0; i < m_camera_datas.size(); i++) {
        const auto& data = m_camera_datas[i];
        cfg.set<float>(std::format("camera_right_offset{}", i), data.offset.x);
        cfg.set<float>(std::format("camera_up_offset{}", i), data.offset.y);
        cfg.set<float>(std::format("camera_forward_offset{}", i), data.offset.z);
        cfg.set<float>(std::format("world_scale{}", i), m_camera_datas[i].world_scale);
        cfg.set<bool>(std::format("decoupled_pitch{}", i), m_camera_datas[i].decoupled_pitch);
        cfg.set<bool>(std::format("decoupled_pitch_ui_adjust{}", i), m_camera_datas[i].decoupled_pitch_ui_adjust);
    }

    cfg.save(cameras_txt.string());
} catch(...) {
    spdlog::error("[VR] Failed to save camera offsets");
}

void VR::save_prospi_camera_calibrations() try {
    ZoneScopedN(__FUNCTION__);

    const auto calibration_path = get_camera_calibration_path();
    std::filesystem::create_directories(calibration_path.parent_path());

    size_t calibration_count{};
    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        calibration_count = m_prospi_camera_calibrations.size();
    }

    if (calibration_count == 0) {
        std::error_code exists_ec{};
        const auto calibration_file_exists = std::filesystem::exists(calibration_path, exists_ec);
        if (!exists_ec && calibration_file_exists) {
            std::error_code remove_ec{};
            std::filesystem::remove(calibration_path, remove_ec);
            if (!remove_ec) {
                spdlog::info("[VR] Cleared camera calibration file {}", calibration_path.string());
            }
        }

        return;
    }

    json data{};
    data["version"] = 1;
    data["cameras"] = json::object();

    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        for (const auto& [camera_id, calibration] : m_prospi_camera_calibrations) {
            data["cameras"][camera_id] = {
                {"camera_id", calibration.camera_id},
                {"preset_name", calibration.preset_name},
                {"actual_min_fov", calibration.actual_min_fov},
                {"dolly_distance", calibration.dolly_distance},
                {"projection_multiplier", calibration.projection_multiplier}
            };
        }
    }

    std::ofstream file{calibration_path};
    file << data.dump(4);

    spdlog::info("[VR] Saved {} camera calibrations to {}", calibration_count, calibration_path.string());
} catch (const std::exception& e) {
    spdlog::error("[VR] Failed to save camera calibrations: {}", e.what());
} catch (...) {
    spdlog::error("[VR] Failed to save camera calibrations");
}

void VR::load_prospi_camera_calibrations() try {
    ZoneScopedN(__FUNCTION__);

    const auto calibration_path = get_camera_calibration_path();
    std::unordered_map<std::string, ProSpiCameraCalibration> calibrations{};

    if (std::filesystem::exists(calibration_path)) {
        std::ifstream file{calibration_path};
        json data{};
        file >> data;

        if (data.contains("cameras") && data["cameras"].is_object()) {
            for (const auto& [camera_id, value] : data["cameras"].items()) {
                if (!value.is_object()) {
                    continue;
                }

                ProSpiCameraCalibration calibration{};
                calibration.camera_id = value.value("camera_id", camera_id);
                calibration.preset_name = value.value("preset_name", std::string{});
                calibration.actual_min_fov = value.value("actual_min_fov", 20.0f);
                calibration.dolly_distance = value.value("dolly_distance", 3000.0f);
                calibration.projection_multiplier = value.value("projection_multiplier", 1.0f);
                calibrations[calibration.camera_id] = calibration;
            }
        }
    }

    size_t calibration_count{};
    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_camera_calibrations = std::move(calibrations);
        calibration_count = m_prospi_camera_calibrations.size();
    }

    spdlog::info("[VR] Loaded {} camera calibrations from {}", calibration_count, calibration_path.string());
} catch (const std::exception& e) {
    spdlog::error("[VR] Failed to load camera calibrations: {}", e.what());
} catch (...) {
    spdlog::error("[VR] Failed to load camera calibrations");
}

void VR::save_current_prospi_camera_calibration() {
    ZoneScopedN(__FUNCTION__);

    const auto camera_id = get_current_prospi_camera_id();
    if (camera_id.empty()) {
        spdlog::warn("[VR] Refusing to save camera calibration without an active camera ID");
        return;
    }

    const auto is_prospi = is_prospi_executable();
    auto preset = (ProSpiCameraPreset)m_match_game_fov_prospi_preset.load(std::memory_order_relaxed);
    auto saved_min_fov = is_prospi ? m_match_game_fov_prospi_actual_min_active.load(std::memory_order_relaxed) : 0.0f;
    if (saved_min_fov <= 0.0f) {
        saved_min_fov = is_prospi ?
            std::clamp(m_match_game_fov_prospi_actual_min->value(), 5.0f, 175.0f) :
            std::clamp(m_match_game_fov_min_enabled->value() ? m_match_game_fov_min->value() : 5.0f, 5.0f, 175.0f);
    }

    ProSpiCameraCalibration calibration{};
    calibration.camera_id = camera_id;
    calibration.preset_name = is_prospi ? get_prospi_camera_preset_name(preset) : std::string{};
    calibration.actual_min_fov = std::clamp(saved_min_fov, 5.0f, 175.0f);
    calibration.dolly_distance = std::clamp(m_match_game_fov_dolly_distance->value(), 10.0f, 50000.0f);
    calibration.projection_multiplier = std::clamp(m_match_game_fov_multiplier->value(), 0.1f, 3.0f);

    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        m_prospi_camera_calibrations[camera_id] = calibration;
    }

    save_prospi_camera_calibrations();

    spdlog::info(
        "[VR] Saved camera calibration camera={} preset={} min={:.2f} mult={:.2f} dolly={:.2f}",
        calibration.camera_id,
        calibration.preset_name.empty() ? "None" : calibration.preset_name,
        calibration.actual_min_fov,
        calibration.projection_multiplier,
        calibration.dolly_distance
    );
}

void VR::clear_current_prospi_camera_calibration() {
    ZoneScopedN(__FUNCTION__);

    const auto camera_id = get_current_prospi_camera_id();
    if (camera_id.empty()) {
        spdlog::warn("[VR] Refusing to clear camera calibration without an active camera ID");
        return;
    }

    bool removed = false;
    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        removed = m_prospi_camera_calibrations.erase(camera_id) > 0;
    }

    if (removed) {
        save_prospi_camera_calibrations();
        spdlog::info("[VR] Cleared camera calibration for camera={}", camera_id);
    }
}

void VR::clear_current_prospi_preset_calibrations() {
    ZoneScopedN(__FUNCTION__);

    const auto preset = (ProSpiCameraPreset)m_match_game_fov_prospi_preset.load(std::memory_order_relaxed);
    const auto preset_name = std::string{get_prospi_camera_preset_name(preset)};
    if (preset == ProSpiCameraPreset::None || preset_name == "None") {
        spdlog::warn("[VR] Refusing to clear ProSpi preset calibrations without an active preset");
        return;
    }

    size_t removed{};
    {
        std::scoped_lock _{m_prospi_camera_calibration_mtx};
        for (auto it = m_prospi_camera_calibrations.begin(); it != m_prospi_camera_calibrations.end();) {
            if (it->second.preset_name == preset_name) {
                it = m_prospi_camera_calibrations.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
    }

    if (removed > 0) {
        save_prospi_camera_calibrations();
        spdlog::info("[VR] Cleared {} ProSpi calibrations for preset={}", removed, preset_name);
    }
}

std::string VR::get_current_prospi_camera_id() {
    std::scoped_lock _{m_prospi_camera_calibration_mtx};
    return m_prospi_current_camera_id;
}

void VR::save_generic_camera_presets() try {
    ZoneScopedN(__FUNCTION__);

    const auto preset_path = get_generic_camera_presets_path();
    std::filesystem::create_directories(preset_path.parent_path());

    size_t preset_count{};
    {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        preset_count = m_generic_camera_presets.size();
    }

    if (preset_count == 0) {
        std::error_code exists_ec{};
        const auto preset_file_exists = std::filesystem::exists(preset_path, exists_ec);
        if (!exists_ec && preset_file_exists) {
            std::error_code remove_ec{};
            std::filesystem::remove(preset_path, remove_ec);
            if (!remove_ec) {
                spdlog::info("[VR] Cleared generic camera preset file {}", preset_path.string());
            }
        }

        return;
    }

    json data{};
    data["version"] = 1;
    data["cameras"] = json::object();

    {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        for (const auto& [camera_id, preset] : m_generic_camera_presets) {
            data["cameras"][camera_id] = {
                {"camera_id", preset.camera_id},
                {"min_fov", preset.min_fov},
                {"dolly_distance", preset.dolly_distance},
                {"projection_multiplier", preset.projection_multiplier},
                {"read_only_camera", preset.read_only_camera}
            };
        }
    }

    std::ofstream file{preset_path};
    file << data.dump(4);

    spdlog::info("[VR] Saved {} generic camera presets to {}", preset_count, preset_path.string());
} catch (const std::exception& e) {
    spdlog::error("[VR] Failed to save generic camera presets: {}", e.what());
} catch (...) {
    spdlog::error("[VR] Failed to save generic camera presets");
}

void VR::load_generic_camera_presets() try {
    ZoneScopedN(__FUNCTION__);

    const auto preset_path = get_generic_camera_presets_path();
    std::unordered_map<std::string, GenericCameraPreset> presets{};

    if (std::filesystem::exists(preset_path)) {
        std::ifstream file{preset_path};
        json data{};
        file >> data;

        if (data.contains("cameras") && data["cameras"].is_object()) {
            for (const auto& [camera_id, value] : data["cameras"].items()) {
                if (!value.is_object()) {
                    continue;
                }

                GenericCameraPreset preset{};
                preset.camera_id = value.value("camera_id", camera_id);
                preset.min_fov = std::clamp(value.value("min_fov", 5.0f), 5.0f, 175.0f);
                preset.dolly_distance = std::clamp(value.value("dolly_distance", 3000.0f), 10.0f, 50000.0f);
                preset.projection_multiplier = std::clamp(value.value("projection_multiplier", 1.0f), 0.1f, 3.0f);
                preset.read_only_camera = value.value("read_only_camera", true);
                presets[preset.camera_id] = preset;
            }
        }
    }

    size_t preset_count{};
    {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_generic_camera_presets = std::move(presets);
        preset_count = m_generic_camera_presets.size();
    }

    spdlog::info("[VR] Loaded {} generic camera presets from {}", preset_count, preset_path.string());
} catch (const std::exception& e) {
    spdlog::error("[VR] Failed to load generic camera presets: {}", e.what());
} catch (...) {
    spdlog::error("[VR] Failed to load generic camera presets");
}

void VR::save_current_generic_camera_preset() {
    ZoneScopedN(__FUNCTION__);

    const auto camera_id = get_current_game_camera_id();
    if (camera_id.empty()) {
        spdlog::warn("[VR] Refusing to save generic camera preset without an active camera ID");
        return;
    }

    GenericCameraPreset preset{};
    preset.camera_id = camera_id;
    preset.min_fov = std::clamp(
        m_match_game_fov_min_enabled->value() ? m_match_game_fov_min->value() : m_game_fov_raw.load(std::memory_order_relaxed),
        5.0f,
        175.0f);
    preset.dolly_distance = std::clamp(m_match_game_fov_dolly_distance->value(), 10.0f, 50000.0f);
    preset.projection_multiplier = std::clamp(m_match_game_fov_multiplier->value(), 0.1f, 3.0f);
    preset.read_only_camera = m_match_game_fov_read_only_camera->value();

    {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        m_generic_camera_presets[camera_id] = preset;
    }

    save_generic_camera_presets();

    spdlog::info(
        "[VR] Saved generic camera preset camera={} min={:.2f} mult={:.2f} dolly={:.2f} read_only={}",
        preset.camera_id,
        preset.min_fov,
        preset.projection_multiplier,
        preset.dolly_distance,
        preset.read_only_camera);
}

void VR::clear_current_generic_camera_preset() {
    ZoneScopedN(__FUNCTION__);

    const auto camera_id = get_current_game_camera_id();
    if (camera_id.empty()) {
        spdlog::warn("[VR] Refusing to clear generic camera preset without an active camera ID");
        return;
    }

    bool removed = false;
    {
        std::scoped_lock _{m_generic_camera_preset_mtx};
        removed = m_generic_camera_presets.erase(camera_id) > 0;
    }

    if (removed) {
        save_generic_camera_presets();
        spdlog::info("[VR] Cleared generic camera preset for camera={}", camera_id);
    }
}

std::string VR::get_current_game_camera_id() {
    std::scoped_lock _{m_generic_camera_preset_mtx};
    return m_current_game_camera_id;
}


void VR::on_pre_imgui_frame() {
    ZoneScopedN(__FUNCTION__);

    m_xinput_context.update();

    if (!get_runtime()->ready()) {
        return;
    }

    if (!m_disable_overlay) {
        m_overlay_component.on_pre_imgui_frame();
    }
}

void VR::handle_keybinds() {
    ZoneScopedN(__FUNCTION__);

    if (m_keybind_recenter->is_key_down_once()) {
        recenter_view();
    }

     if (m_keybind_recenter_horizon->is_key_down_once()) {
        recenter_horizon();
    }	
    	
    if (m_keybind_load_camera_0->is_key_down_once()) {
        load_camera(0);
    }

    if (m_keybind_load_camera_1->is_key_down_once()) {
        load_camera(1);
    }

    if (m_keybind_load_camera_2->is_key_down_once()) {
        load_camera(2);
    }

    if (m_keybind_set_standing_origin->is_key_down_once()) {
        m_standing_origin = get_position(0);
    }

    if (m_keybind_toggle_2d_screen->is_key_down_once()) {
        m_2d_screen_mode->toggle();
    }

    if (m_keybind_disable_vr->is_key_down_once()) {
        m_disable_vr = !m_disable_vr; // definitely should not be persistent
    }

    // The Slate UI
    if (m_keybind_toggle_gui->is_key_down_once()) {
        m_enable_gui->toggle();
    }
}

void VR::on_frame() {
    ZoneScopedN(__FUNCTION__);

    m_last_mod_frame = std::chrono::steady_clock::now();
    m_cvar_manager->on_frame();
    handle_keybinds();

    if (!get_runtime()->ready()) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto is_allowed_draw_window = now - m_last_xinput_update < std::chrono::seconds(2);

    if (!is_allowed_draw_window) {
        m_rt_modifier.draw = false;
    }

    if (is_allowed_draw_window && m_xinput_context.headlocked_begin_held && !FrameworkConfig::get()->is_l3_r3_long_press()) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("AimMethod Notification", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

        ImGui::Text("Continue holding down L3 + R3 to toggle aim method");

        if (std::chrono::steady_clock::now() - m_xinput_context.headlocked_begin >= std::chrono::seconds(1)) {
            if (m_aim_method->value() == VR::AimMethod::GAME) {
                m_aim_method->value() = m_previous_aim_method;
            } else {
                m_aim_method->value() = VR::AimMethod::GAME; // turns it off
            }

            m_xinput_context.headlocked_begin_held = false;
        } else {
            if (m_aim_method->value() != VR::AimMethod::GAME) {
                m_previous_aim_method = (VR::AimMethod)m_aim_method->value();
            } else if (m_previous_aim_method == VR::AimMethod::GAME) {
                m_previous_aim_method = VR::AimMethod::HEAD; // so it will at least be something
            }
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);

        ImGui::End();
    }

    if (m_rt_modifier.draw) {
        const auto rt_size = g_framework->get_rt_size();

        ImGui::Begin("RT Modifier Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Left Stick: Camera left/right/forward/back");
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Right Stick: Camera up/down");
        
        ImGui::Text("Page: %d", m_rt_modifier.page + 1);
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "DPad Left: Previous page | DPad Right: Next page");

        switch (m_rt_modifier.page) {
        case 2:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Save Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Save Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Save Camera 0");
            break;

        case 1:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Load Camera 2");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Load Camera 1");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Load Camera 0");
            break;

        case 0:
        default:
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + B: Reset camera offset");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + Y: Recenter view");
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "RT + X: Reset standing origin");
            m_rt_modifier.page = 0;
            break;
        }

        const auto window_size = ImGui::GetWindowSize();

        const auto centered_x = (rt_size.x / 2) - (window_size.x / 2);
        const auto centered_y = (rt_size.y / 2) - (window_size.y / 2);
        ImGui::SetWindowPos(ImVec2(centered_x, centered_y), ImGuiCond_Always);
        ImGui::End();
    }
}

void VR::on_present() {
    ZoneScopedN(__FUNCTION__);

    m_present_thread_id = GetCurrentThreadId();

    utility::ScopeGuard _guard {[&]() {
        if (!is_using_afr() || (m_render_frame_count + 1) % 2 == m_left_eye_interval) {
            SetEvent(m_present_finished_event);
        }

        m_last_frame_count = m_render_frame_count;
    }};

    m_frame_count = get_runtime()->internal_render_frame_count;

    if (!is_using_afr() || m_render_frame_count % 2 == m_left_eye_interval) {
        ResetEvent(m_present_finished_event);
    }

    auto runtime = get_runtime();

    if (!runtime->loaded) {
        m_fake_stereo_hook->on_frame(); // Just let all the hooks engage, whatever.
        return;
    }

    runtime->consume_events(nullptr);

    m_fake_stereo_hook->on_frame();

    auto openvr = get_runtime<runtimes::OpenVR>();

    if (runtime->is_openvr()) {
        if (openvr->got_first_poses) {
            const auto hmd_activity = openvr->hmd->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
            auto hmd_active = hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction || hmd_activity == vr::k_EDeviceActivityLevel_UserInteraction_Timeout;

            if (hmd_active) {
                openvr->last_hmd_active_time = std::chrono::system_clock::now();
            }

            const auto now = std::chrono::system_clock::now();

            if (now - openvr->last_hmd_active_time <= std::chrono::seconds(5)) {
                hmd_active = true;
            }

            openvr->is_hmd_active = hmd_active;

            // upon headset re-entry, reinitialize OpenVR
            if (openvr->is_hmd_active && !openvr->was_hmd_active) {
                openvr->wants_reinitialize = true;
            }

            openvr->was_hmd_active = openvr->is_hmd_active;

            if (!is_hmd_active()) {
                return;
            }
        } else {
            openvr->is_hmd_active = true; // We need to force out an initial WaitGetPoses call
            openvr->was_hmd_active = true;
        }
    }

    // attempt to fix crash when reinitializing openvr
    std::scoped_lock _{m_openvr_mtx};
    m_submitted = false;

    const auto renderer = g_framework->get_renderer_type();
    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    const auto is_left_eye_frame = is_using_afr() ? (m_render_frame_count % 2 == m_left_eye_interval) : true;

    if (is_left_eye_frame && get_synchronize_stage() == VR::SynchronizeStage::LATE) {
        const auto had_sync = runtime->got_first_sync;
        runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::VRLateOnPresent);

        if (!runtime->got_first_poses || !had_sync) {
            update_hmd_state();
        }
    }

    if (renderer == Framework::RendererType::D3D11) {
        // if we don't do this then D3D11 OpenXR freezes for some reason.
        if (!runtime->got_first_sync) {
            SPDLOG_INFO_EVERY_N_SEC(1, "Attempting to sync!");
            if (get_synchronize_stage() == VR::SynchronizeStage::LATE) {
                runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::VRD3D11InitialSync);
            }

            update_hmd_state();
        }

        m_is_d3d12 = false;
        e = m_d3d11.on_frame(this);
    } else if (renderer == Framework::RendererType::D3D12) {
        m_is_d3d12 = true;
        e = m_d3d12.on_frame(this);
    }

    // force a waitgetposes call to fix this...
    if (e == vr::EVRCompositorError::VRCompositorError_AlreadySubmitted && runtime->is_openvr()) {
        openvr->got_first_poses = false;
        openvr->needs_pose_update = true;
    }

    if (m_submitted) {
        if (m_submitted) {
            if (!m_disable_overlay) {
                m_overlay_component.on_post_compositor_submit();
            }

            if (runtime->is_openvr()) {
                //vr::VRCompositor()->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
                //vr::VRCompositor()->PostPresentHandoff();
            }
        }

        //runtime->needs_pose_update = true;
        m_submitted = false;

        // On the first ever submit, we need to activate the window and set the mouse to the center
        // so the user doesn't have to click on the window to get input.
        if (m_first_submit) {
            m_first_submit = false;
            const auto skip_initial_mouse_warp = is_ue418_executable() && should_skip_post_init_properties();

            // for some reason this doesn't work if called directly from here
            // so we have to do it in a separate thread
            std::thread worker([skip_initial_mouse_warp]() {
                if (!skip_initial_mouse_warp) {
                    g_framework->activate_window();
                    g_framework->set_mouse_to_center();
                } else {
                    spdlog::info("Skipping first-submit window activation/mouse recenter for UE4.18 SkipPostInitProperties compatibility");
                }

                spdlog::info("Finished first submit from worker thread!");
            });
            worker.detach();
        }
    }
}

void VR::on_post_present() {
    FrameMarkNamed("Present");
    ZoneScopedN(__FUNCTION__);

    const auto is_same_frame = m_render_frame_count > 0 && m_render_frame_count == m_frame_count;

    m_render_frame_count = m_frame_count;

    auto runtime = get_runtime();

    if (!get_runtime()->loaded) {
        return;
    }

    std::scoped_lock _{m_openvr_mtx};

    if (!m_is_d3d12) {
        m_d3d11.on_post_present(this);
    } else {
        m_d3d12.on_post_present(this);
    }

    detect_controllers();

    const auto is_left_eye_frame = is_using_afr() ? (is_same_frame || (m_render_frame_count % 2 == m_left_eye_interval)) : true;

    if (is_left_eye_frame) {
        const auto should_defer_very_late_wait =
            get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE &&
            should_defer_stalker2_very_late_openxr_wait(runtime, m_is_d3d12);

        if (!should_defer_very_late_wait && (get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE || !runtime->got_first_sync)) {
            const auto had_sync = runtime->got_first_sync;
            const auto callsite = get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE
                ? VRRuntime::SyncFrameCallsite::VRVeryLatePostPresent
                : VRRuntime::SyncFrameCallsite::VRPostPresentInitialSync;
            runtime->synchronize_frame(std::nullopt, callsite);

            if (!runtime->got_first_poses || !had_sync) {
                update_hmd_state();
            }
        } else if (should_defer_very_late_wait) {
            SPDLOG_INFO_ONCE("[Stalker2][OpenXR] Deferring VERY_LATE xrWaitFrame to the D3D12 submit path after initial valid poses");
        }

        if (runtime->is_openxr() && m_openxr->can_run_frame_loop() && get_synchronize_stage() > VR::SynchronizeStage::EARLY) {
            if (!m_is_d3d12 && !m_openxr->frame_began) {
                m_openxr->begin_frame("vr_post_present");
            }
        }
    }

    if (runtime->wants_reinitialize) {
        std::scoped_lock _{m_reinitialize_mtx};

        if (runtime->is_openvr()) {
            m_openvr->wants_reinitialize = false;
            reinitialize_openvr();
        } else if (runtime->is_openxr()) {
            m_openxr->wants_reinitialize = false;
            reinitialize_openxr();
        }
    }
}

uint32_t VR::get_hmd_width() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().x * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().x;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().x;
    }

    return std::max<uint32_t>(get_runtime()->get_width(), 128);
}

uint32_t VR::get_hmd_height() const {
    if (m_2d_screen_mode->value()) {
        if (get_runtime()->is_openxr()) {
            return g_framework->get_rt_size().y * m_openxr->resolution_scale->value();
        }

        return g_framework->get_rt_size().y;
    }

    if (m_extreme_compat_mode->value()) {
        return g_framework->get_rt_size().y;
    }

    return std::max<uint32_t>(get_runtime()->get_height(), 128);
}

void VR::on_draw_sidebar_entry(std::string_view name) {
    const auto hash = utility::hash(name.data());

    // Draw the ui thats always drawn first.
    on_draw_ui();

    /*const auto made_child = ImGui::BeginChild("VRChild", ImVec2(0, 0), true, ImGuiWindowFlags_::ImGuiWindowFlags_NavFlattened);

    utility::ScopeGuard sg([made_child]() {
        if (made_child) {
            ImGui::EndChild();
        }
    });*/

    enum SelectedPage {
        PAGE_RUNTIME,
        PAGE_UNREAL,
        PAGE_INPUT,
        PAGE_CAMERA,
        PAGE_KEYBINDS,
        PAGE_CONSOLE,
        PAGE_COMPATIBILITY,
        PAGE_DEBUG,
    };

    SelectedPage selected_page = PAGE_RUNTIME;

    /*ImGui::BeginTable("VRTable", 2, ImGuiTableFlags_::ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_::ImGuiTableFlags_BordersOuterV | ImGuiTableFlags_::ImGuiTableFlags_SizingFixedFit);
    ImGui::TableSetupColumn("LeftPane", ImGuiTableColumnFlags_WidthFixed, 150.0f);
    ImGui::TableSetupColumn("RightPane", ImGuiTableColumnFlags_WidthStretch);

    // Draw left pane
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); // Set to the first column

        ImGui::BeginGroup();

        auto dcs = [&](const char* label, SelectedPage page_value) -> bool {
            ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
            utility::ScopeGuard sg3([]() {
                ImGui::PopStyleVar();
            });
            if (ImGui::Selectable(label, selected_page == page_value)) {
                selected_page = page_value;
                return true;
            }
            return false;
        };

        dcs("Runtime", PAGE_RUNTIME);
        dcs("Unreal", PAGE_UNREAL);
        dcs("Input", PAGE_INPUT);
        dcs("Camera", PAGE_CAMERA);
        dcs("Console/CVars", PAGE_CONSOLE);
        dcs("Compatibility", PAGE_COMPATIBILITY);
        dcs("Debug", PAGE_DEBUG);

        ImGui::EndGroup();
    }

    ImGui::TableNextColumn(); // Move to the next column (right)
    ImGui::BeginGroup();*/

    switch (hash) {
    case "Runtime"_fnv:
        selected_page = PAGE_RUNTIME;
        break;
    case "Unreal"_fnv:
        selected_page = PAGE_UNREAL;
        break;
    case "Input"_fnv:
        selected_page = PAGE_INPUT;
        break;
    case "Camera"_fnv:
        selected_page = PAGE_CAMERA;
        break;
    case "Keybinds"_fnv:
        selected_page = PAGE_KEYBINDS;
        break;
    case "Console/CVars"_fnv:
        selected_page = PAGE_CONSOLE;
        break;
    case "Compatibility"_fnv:
        selected_page = PAGE_COMPATIBILITY;
        break;
    case "Debug"_fnv:
        selected_page = PAGE_DEBUG;
        break;
    default:
        ImGui::Text("Unknown page selected");
        break;
    }

    if (selected_page == PAGE_RUNTIME) {
        if (m_has_hw_scheduling) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: Hardware-accelerated GPU scheduling is enabled. This may cause the game to run slower.");
            ImGui::TextWrapped("Go into your Windows Graphics settings and disable \"Hardware-accelerated GPU scheduling\"");
            ImGui::PopStyleColor();
            ImGui::TextWrapped("Note: This is only necessary if you are experiencing performance issues.");
        }

        if (GetModuleHandleW(L"nvngx_dlssg.dll") != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.0f, 0.0f, 1.0f));
            ImGui::TextWrapped("WARNING: DLSS Frame Generation has been detected. Make sure it is disabled within in-game settings.");
            ImGui::PopStyleColor();
        }

        ImGui::Text((std::string{"Runtime Information ("} + get_runtime()->name().data() + ")").c_str());

        m_desktop_fix->draw("Desktop Spectator View");

        if (m_desktop_fix->value()) {
            m_desktop_mirror_mode->draw("Desktop Spectator View Mode");
        }

        m_2d_screen_mode->draw("2D Screen Mode");

        ImGui::TextWrapped("Render Resolution (per-eye): %d x %d", get_runtime()->get_width(), get_runtime()->get_height());
        ImGui::TextWrapped("Total Render Resolution: %d x %d", get_runtime()->get_width() * 2, get_runtime()->get_height());

        if (get_runtime()->is_openvr()) {
            ImGui::TextWrapped("Resolution can be changed in SteamVR");
        }

        get_runtime()->on_draw_ui();

        m_overlay_component.on_draw_ui();

        ImGui::TreePop();
    }

    if (selected_page == PAGE_UNREAL) {
        m_rendering_method->draw("Rendering Method");
        m_synced_afr_method->draw("Synced Sequential Method");

        m_world_scale->draw("World Scale");
        m_depth_scale->draw("Depth Scale");

        m_disable_hzbocclusion->draw("Disable HZBOcclusion");
        m_disable_instance_culling->draw("Disable Instance Culling");
        m_disable_hdr_compositing->draw("Disable HDR Composition");
        m_disable_blur_widgets->draw("Disable Blur Widgets");
        m_uncap_framerate->draw("Uncap Framerate");
        m_enable_gui->draw("Enable GUI");
        m_enable_depth->draw("Enable Depth-based Latency Reduction");
        m_load_blueprint_code->draw("Load Blueprint Code");
        m_ghosting_fix->draw("Ghosting Fix");

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Native Stereo Fix")) {
            m_native_stereo_fix->draw("Enabled");
            if (should_force_native_stereo_fix_same_pass()) {
                m_native_stereo_fix_same_pass->value() = true;
                ImGui::BeginDisabled();
                m_native_stereo_fix_same_pass->draw("Use Same Stereo Pass");
                ImGui::EndDisabled();
                ImGui::TextWrapped("Forced for Stalker2 stability while Native Stereo Fix is enabled.");
            } else {
                m_native_stereo_fix_same_pass->draw("Use Same Stereo Pass");
            }
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Near Clip Plane")) {
            m_custom_z_near_enabled->draw("Enable");

            if (m_custom_z_near_enabled->value()) {
                m_custom_z_near->draw("Value");

                if (m_custom_z_near->value() <= 0.0f) {
                    m_custom_z_near->value() = 0.01f;
                }
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_INPUT) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Controller")) {
            m_joystick_deadzone->draw("VR Joystick Deadzone");
            m_controller_pitch_offset->draw("Controller Pitch Offset");

            m_dpad_shifting->draw("DPad Shifting");
            ImGui::SameLine();
            m_swap_controllers->draw("Left-handed Controller Inputs");
            m_dpad_shifting_method->draw("DPad Shifting Method");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Aim Method")) {
            ImGui::TextWrapped("Some games may not work with this enabled.");
            if (m_aim_method->draw("Type")) {
                m_previous_aim_method = (AimMethod)m_aim_method->value();
            }

            m_aim_speed->draw("Speed");
            m_aim_interp->draw("Smoothing");

            m_aim_modify_player_control_rotation->draw("Modify Player Control Rotation");
            ImGui::SameLine();
            m_aim_use_pawn_control_rotation->draw("Use Pawn Control Rotation");

            m_aim_multiplayer_support->draw("Multiplayer Support");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Snap Turn")) {
            m_snapturn->draw("Enabled");
            m_snapturn_angle->draw("Angle");
            m_snapturn_joystick_deadzone->draw("Deadzone");
        
            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Movement Orientation")) {
            m_movement_orientation->draw("Type");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Roomscale Movement")) {
            m_roomscale_movement->draw("Enabled");

            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, headset movement will affect the movement of the player character.");
            }

            ImGui::SameLine();
            m_roomscale_sweep->draw("Sweep Movement");
            // Draw description of option
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, roomscale movement will use a sweep to prevent the player from moving through walls.\nThis also allows physics objects to interact with the player, like doors.");
            }

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_CAMERA) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Freeze")) {
            float camera_offset[] = {m_camera_forward_offset->value(), m_camera_right_offset->value(), m_camera_up_offset->value()};
            if (ImGui::SliderFloat3("Camera Offset", camera_offset, -4000.0f, 4000.0f)) {
                m_camera_forward_offset->value() = camera_offset[0];
                m_camera_right_offset->value() = camera_offset[1];
                m_camera_up_offset->value() = camera_offset[2];
            }

            for (auto i = 0; i < m_camera_datas.size(); ++i) {
                auto& data = m_camera_datas[i];

                if (ImGui::Button(std::format("Save Camera {}", i).data())) {
                    save_camera(i);
                }

                ImGui::SameLine();

                if (ImGui::Button(std::format("Load Camera {}", i).data())) {
                    load_camera(i);
                }
            }

            bool pos_freeze = m_camera_freeze.position_frozen || m_camera_freeze.position_wants_freeze;
            if (ImGui::Checkbox("Freeze Position", &pos_freeze)) {
                if (pos_freeze) {
                    m_camera_freeze.position_wants_freeze = true;
                } else {
                    m_camera_freeze.position_frozen = false;
                }
            }

            ImGui::SameLine();
            bool rot_freeze = m_camera_freeze.rotation_frozen || m_camera_freeze.rotation_wants_freeze;
            if (ImGui::Checkbox("Freeze Rotation", &rot_freeze)) {
                if (rot_freeze) {
                    m_camera_freeze.rotation_wants_freeze = true;
                } else {
                    m_camera_freeze.rotation_frozen = false;
                }
            }

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Game FOV")) {
            m_match_game_fov->draw("Match Game FOV");

            if (m_match_game_fov->value()) {
                if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_dolly->draw("Use Dolly Instead of FOV");
                    m_match_game_fov_multiplier->draw("FOV Multiplier");
                    m_match_game_fov_min_enabled->draw("Clamp Minimum FOV");
                    if (m_match_game_fov_min_enabled->value()) {
                        m_match_game_fov_min->draw("Minimum FOV");
                    }
                    m_match_game_fov_read_only_camera->draw("Read Game Camera Only (No FOV Writes)");

                    if (m_match_game_fov_dolly->value()) {
                        m_match_game_fov_dolly_distance->draw_drag("Dolly Focus Distance", 10.0f, "%.0f");
                        ImGui::Text("Dolly Offset: %.2f", get_game_fov_dolly_offset());
                    }
                }

                if (ImGui::CollapsingHeader("Camera Cut Stabilizer")) {
                    m_match_game_fov_camera_cut_stabilizer->draw("Enable Camera Cut Stabilizer");
                    if (m_match_game_fov_camera_cut_stabilizer->value()) {
                        m_match_game_fov_camera_cut_stabilizer_duration_ms->draw("Stabilizer Duration (ms)");
                        m_match_game_fov_camera_cut_stabilizer_fov_delta->draw("Cut FOV Delta Threshold");
                        m_match_game_fov_camera_cut_stabilizer_rotation_delta->draw("Cut Rotation Delta Threshold");
                        m_match_game_fov_camera_cut_stabilizer_location_delta->draw("Cut Location Delta Threshold");
                    }
                }

                if (m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("Generic Camera Presets")) {
                    m_match_game_fov_generic_camera_presets->draw("Enable Generic Camera Presets");
                    if (m_match_game_fov_generic_camera_presets->value()) {
                        m_match_game_fov_generic_camera_presets_auto_apply->draw("Auto Apply Saved Camera Presets");

                        if (ImGui::Button("Save Current Generic Camera Preset")) {
                            save_current_generic_camera_preset();
                        }

                        ImGui::SameLine();

                        if (ImGui::Button("Clear Current Generic Camera Preset")) {
                            clear_current_generic_camera_preset();
                        }
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Actual FOV Clamp", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_actual_clamp->draw("Clamp Actual Game FOV (ProSpi)");
                    if (m_match_game_fov_prospi_actual_clamp->value()) {
                        m_match_game_fov_prospi_actual_min->draw("Default ProSpi Actual Minimum FOV");
                        m_match_game_fov_prospi_center_field_actual_min->draw("Center Field Minimum FOV");
                        m_match_game_fov_prospi_upper_deck_actual_min->draw("Upper Deck Minimum FOV");
                        m_match_game_fov_prospi_plate_high_actual_min->draw("High Plate Minimum FOV");
                        m_match_game_fov_prospi_deep_outfield_actual_min->draw("Deep Outfield Minimum FOV");
                    }
                }

                if (is_prospi_executable() &&
                    ImGui::CollapsingHeader("ProSpi Telephoto Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_telephoto_perf_override->draw("Enable Telephoto Performance Override");
                    if (m_match_game_fov_prospi_telephoto_perf_override->value()) {
                        m_match_game_fov_prospi_telephoto_perf_trigger_fov->draw("Telephoto Trigger FOV");
                        m_match_game_fov_prospi_telephoto_perf_view_distance_scale->draw("Telephoto View Distance Scale");
                        m_match_game_fov_prospi_telephoto_perf_static_mesh_lod_distance_scale->draw("Telephoto Static Mesh LOD Distance Scale");
                        m_match_game_fov_prospi_telephoto_perf_skeletal_mesh_lod_bias->draw("Telephoto Skeletal Mesh LOD Bias");
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Dolly Overrides", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_tv_dolly_override->draw("Auto Override TV View Dolly");
                    if (m_match_game_fov_prospi_tv_dolly_override->value()) {
                        m_match_game_fov_prospi_tv_dolly_distance->draw_drag("TV View Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_opening_aerial_dolly_override->draw("Auto Override Opening Aerial Dolly");
                    if (m_match_game_fov_prospi_opening_aerial_dolly_override->value()) {
                        m_match_game_fov_prospi_opening_aerial_dolly_distance->draw_drag("Opening Aerial Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_behind_plate_wide_dolly_override->draw("Auto Override Behind Plate Wide Dolly");
                    if (m_match_game_fov_prospi_behind_plate_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_behind_plate_wide_dolly_distance->draw_drag("Behind Plate Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->draw("Auto Override Waist High Reverse Dolly");
                    if (m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_waist_high_reverse_dolly_distance->draw_drag("Waist High Reverse Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_low_plate_corner_dolly_override->draw("Auto Override Low Infield Close-Up Dolly");
                    if (m_match_game_fov_prospi_low_plate_corner_dolly_override->value()) {
                        m_match_game_fov_prospi_low_plate_corner_dolly_distance->draw_drag("Low Infield Close-Up Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_center_field_dolly_override->draw("Auto Override Center Field Dolly");
                    if (m_match_game_fov_prospi_center_field_dolly_override->value()) {
                        m_match_game_fov_prospi_center_field_dolly_distance->draw_drag("Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_center_field_high_dolly_override->draw("Auto Override Center Field High Dolly");
                    if (m_match_game_fov_prospi_center_field_high_dolly_override->value()) {
                        m_match_game_fov_prospi_center_field_high_dolly_distance->draw_drag("Center Field High Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_left_field_corner_wide_dolly_override->draw("Auto Override Offset Center Field Dolly");
                    if (m_match_game_fov_prospi_left_field_corner_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_left_field_corner_wide_dolly_distance->draw_drag("Offset Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_deep_outfield_dolly_override->draw("Auto Override Deep Outfield Dolly");
                    if (m_match_game_fov_prospi_deep_outfield_dolly_override->value()) {
                        m_match_game_fov_prospi_deep_outfield_dolly_distance->draw_drag("Deep Outfield Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_sky_dolly_override->draw("Auto Override Home Plate Sky Dolly");
                    if (m_match_game_fov_prospi_home_plate_sky_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_sky_dolly_distance->draw_drag("Home Plate Sky Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_upper_deck_dolly_override->draw("Auto Override Upper Deck 3B Dolly");
                    if (m_match_game_fov_prospi_upper_deck_dolly_override->value()) {
                        m_match_game_fov_prospi_upper_deck_dolly_distance->draw_drag("Upper Deck 3B Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_sky_dolly_override->draw("Auto Override Home Sky Dolly");
                    if (m_match_game_fov_prospi_home_sky_dolly_override->value()) {
                        m_match_game_fov_prospi_home_sky_dolly_distance->draw_drag("Home Sky Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_dolly_override->draw("Auto Override Third Base Line Dolly");
                    if (m_match_game_fov_prospi_third_base_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_dolly_distance->draw_drag("Third Base Line Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_relay_low_dolly_override->draw("Auto Override Third Base Relay Low Dolly");
                    if (m_match_game_fov_prospi_third_base_relay_low_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_relay_low_dolly_distance->draw_drag("Third Base Relay Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_sweep_dolly_override->draw("Auto Override Third Base Corner Low Dolly");
                    if (m_match_game_fov_prospi_third_base_sweep_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_sweep_dolly_distance->draw_drag("Third Base Corner Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_third_base_wide_dolly_override->draw("Auto Override Third Base Wide Dolly");
                    if (m_match_game_fov_prospi_third_base_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_third_base_wide_dolly_distance->draw_drag("Third Base Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_dolly_override->draw("Auto Override First Base Line Dolly");
                    if (m_match_game_fov_prospi_first_base_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_dolly_distance->draw_drag("First Base Line Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_wide_dolly_override->draw("Auto Override First Base Wide Dolly");
                    if (m_match_game_fov_prospi_first_base_wide_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_wide_dolly_distance->draw_drag("First Base Wide Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_first_base_corner_low_dolly_override->draw("Auto Override First Base Corner Low Dolly");
                    if (m_match_game_fov_prospi_first_base_corner_low_dolly_override->value()) {
                        m_match_game_fov_prospi_first_base_corner_low_dolly_distance->draw_drag("First Base Corner Low Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_backstop_high_dolly_override->draw("Auto Override Backstop High Dolly");
                    if (m_match_game_fov_prospi_backstop_high_dolly_override->value()) {
                        m_match_game_fov_prospi_backstop_high_dolly_distance->draw_drag("Backstop High Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_right_field_corner_dolly_override->draw("Auto Override Right Field Corner Dolly");
                    if (m_match_game_fov_prospi_right_field_corner_dolly_override->value()) {
                        m_match_game_fov_prospi_right_field_corner_dolly_distance->draw_drag("Right Field Corner Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_right_center_field_dolly_override->draw("Auto Override Right Center Field Dolly");
                    if (m_match_game_fov_prospi_right_center_field_dolly_override->value()) {
                        m_match_game_fov_prospi_right_center_field_dolly_distance->draw_drag("Right Center Field Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_plate_high_dolly_override->draw("Auto Override High Plate Dolly");
                    if (m_match_game_fov_prospi_plate_high_dolly_override->value()) {
                        m_match_game_fov_prospi_plate_high_dolly_distance->draw_drag("High Plate Dolly Distance", 10.0f, "%.0f");
                    }
                    m_match_game_fov_prospi_home_plate_overhead_dolly_override->draw("Auto Override Home Plate Overhead Dolly");
                    if (m_match_game_fov_prospi_home_plate_overhead_dolly_override->value()) {
                        m_match_game_fov_prospi_home_plate_overhead_dolly_distance->draw_drag("Home Plate Overhead Dolly Distance", 10.0f, "%.0f");
                    }
                }

                if (m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("Camera Calibration", ImGuiTreeNodeFlags_DefaultOpen)) {
                    m_match_game_fov_prospi_camera_calibration_auto->draw("Auto Apply Camera Calibration");
                    if (ImGui::Button("Save Current Camera Calibration")) {
                        save_current_prospi_camera_calibration();
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Clear Current Camera Calibration")) {
                        clear_current_prospi_camera_calibration();
                    }

                    if (is_prospi_executable()) {
                        ImGui::SameLine();

                        if (ImGui::Button("Clear Current Preset Calibrations")) {
                            clear_current_prospi_preset_calibrations();
                        }
                    }
                }

                if (ImGui::CollapsingHeader("Live Status", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const auto fov = get_game_fov();
                    const auto raw_fov = m_game_fov_raw.load(std::memory_order_relaxed);
                    const bool fov_valid = m_game_fov_valid.load(std::memory_order_relaxed);
                    const auto current_camera_id = get_current_game_camera_id();
                    const auto calibration_applied = m_match_game_fov_prospi_calibration_applied.load(std::memory_order_relaxed);
                    const auto calibration_multiplier = m_match_game_fov_prospi_calibration_multiplier_active.load(std::memory_order_relaxed);
                    const auto calibration_dolly_distance = m_match_game_fov_prospi_calibration_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto read_only_active = m_match_game_fov_read_only_camera_active.load(std::memory_order_relaxed);
                    const auto would_write = m_match_game_fov_would_write_game_camera.load(std::memory_order_relaxed);
                    const auto stabilizer_active = m_match_game_fov_camera_cut_stabilizer_active.load(std::memory_order_relaxed);
                    const auto stabilizer_remaining_ms = m_match_game_fov_camera_cut_stabilizer_remaining_ms.load(std::memory_order_relaxed);
                    const auto generic_preset_applied = m_match_game_fov_generic_camera_preset_applied.load(std::memory_order_relaxed);
                    ImGui::Text("Current Game FOV: %.2f (%s)", fov, fov_valid ? "valid" : "invalid");
                    ImGui::Text("Raw Game FOV: %.2f", raw_fov);
                    ImGui::Text("Current Camera ID: %s", current_camera_id.empty() ? "None" : current_camera_id.c_str());
                    ImGui::Text("Read-Only Camera Active: %s", read_only_active ? "yes" : "no");
                    ImGui::Text("Blocked Game FOV Write Pending: %s", would_write ? "yes" : "no");
                    ImGui::Text("Camera Cut Stabilizer: %s (%dms)", stabilizer_active ? "active" : "inactive", stabilizer_remaining_ms);
                    ImGui::Text("Generic Camera Preset Applied: %s", generic_preset_applied ? "yes" : "no");
                    ImGui::Text("Camera Calibration Applied: %s", calibration_applied ? "yes" : "no");
                    if (calibration_applied) {
                        ImGui::Text("Calibration Multiplier: %.2f", calibration_multiplier);
                        ImGui::Text("Calibration Dolly Distance: %.2f", calibration_dolly_distance);
                    }
                }

                if (is_prospi_executable() && m_match_game_fov_dolly->value() &&
                    ImGui::CollapsingHeader("ProSpi Live Status", ImGuiTreeNodeFlags_DefaultOpen)) {
                    const auto preset = (ProSpiCameraPreset)m_match_game_fov_prospi_preset.load(std::memory_order_relaxed);
                    const auto active_min = m_match_game_fov_prospi_actual_min_active.load(std::memory_order_relaxed);
                    const auto calibration_applied = m_match_game_fov_prospi_calibration_applied.load(std::memory_order_relaxed);
                    const auto calibration_min = m_match_game_fov_prospi_calibration_actual_min_active.load(std::memory_order_relaxed);
                    const auto calibration_multiplier = m_match_game_fov_prospi_calibration_multiplier_active.load(std::memory_order_relaxed);
                    const auto calibration_dolly_distance = m_match_game_fov_prospi_calibration_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto tv_override_applied = m_match_game_fov_prospi_tv_override_active.load(std::memory_order_relaxed);
                    const auto auto_dolly_distance = m_match_game_fov_prospi_auto_dolly_distance_active.load(std::memory_order_relaxed);
                    const auto telephoto_perf_active = m_match_game_fov_prospi_telephoto_perf_active.load(std::memory_order_relaxed);
                    const auto current_camera_id = get_current_prospi_camera_id();
                    ImGui::Text("Active ProSpi Preset: %s", get_prospi_camera_preset_name(preset));
                    if (m_match_game_fov_prospi_actual_clamp->value()) {
                        ImGui::Text("Active ProSpi Minimum FOV: %.2f", active_min);
                    }
                    ImGui::Text("Current ProSpi Camera ID: %s", current_camera_id.empty() ? "None" : current_camera_id.c_str());
                    ImGui::Text("Calibration Applied: %s", calibration_applied ? "yes" : "no");
                    ImGui::Text("TV Override Active: %s", tv_override_applied ? "yes" : "no");
                    ImGui::Text("Telephoto Performance Override: %s", telephoto_perf_active ? "yes" : "no");
                    ImGui::Text("Auto Dolly Override Distance: %.2f", auto_dolly_distance);
                    if (calibration_applied) {
                        ImGui::Text("Calibration Minimum FOV: %.2f", calibration_min);
                        ImGui::Text("Calibration Multiplier: %.2f", calibration_multiplier);
                        ImGui::Text("Calibration Dolly Distance: %.2f", calibration_dolly_distance);
                    }
                }
            }

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Lerp")) {
            m_lerp_camera_pitch->draw("Lerp Pitch");
            ImGui::SameLine();
            m_lerp_camera_yaw->draw("Lerp Yaw");
            ImGui::SameLine();
            m_lerp_camera_roll->draw("Lerp Roll");
            m_lerp_camera_speed->draw("Lerp Speed");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Decoupled Pitch")) {
            m_decoupled_pitch->draw("Enabled");
            m_decoupled_pitch_ui_adjust->draw("Auto Adjust UI");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_KEYBINDS) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Playspace Keys")) {
            m_keybind_recenter->draw("Recenter View Key");
	    m_keybind_recenter_horizon->draw("Recenter Horizon Key");
            m_keybind_set_standing_origin->draw("Set Standing Origin Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Camera Keys")) {
            m_keybind_load_camera_0->draw("Load Camera 0 Key");
            m_keybind_load_camera_1->draw("Load Camera 1 Key");
            m_keybind_load_camera_2->draw("Load Camera 2 Key");

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Overlay/Runtime Keys")) {
            m_keybind_toggle_2d_screen->draw("Toggle 2D Screen Mode Key");
            m_keybind_toggle_gui->draw("Toggle In-Game UI Key");
            m_keybind_disable_vr->draw("Disable VR Key");

            ImGui::TreePop();
        }
    }

    if (selected_page == PAGE_CONSOLE) {
        m_cvar_manager->on_draw_ui();
    }

    if (selected_page == PAGE_COMPATIBILITY) {
        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Compatibility Options")) {
            m_compatibility_ahud->draw("AHUD UI Compatibility");
            m_compatibility_skip_uobjectarray_init->draw("Skip UObjectArray Init");
            m_compatibility_skip_pip->draw("Skip PostInitProperties");
            m_compatibility_direct_aim->draw("Direct Aim Fallback");
            m_compatibility_controller_camera_guard->draw("Controller-Camera Conflict Guard");
            m_sceneview_compatibility_mode->draw("SceneView Compatibility Mode");
            m_extreme_compat_mode->draw("Extreme Compatibility Mode");

            // changes to any of these options should trigger a regeneration of the eye projection matrices
            const auto horizontal_projection_changed = m_horizontal_projection_override->draw("Horizontal Projection");
            const auto vertical_projection_changed = m_vertical_projection_override->draw("Vertical Projection");
            const auto scale_render = m_grow_rectangle_for_projection_cropping->draw("Scale Render Target");
            const auto scale_render_changed = get_runtime()->is_modifying_eye_texture_scale != scale_render;
            get_runtime()->is_modifying_eye_texture_scale = scale_render;
            get_runtime()->should_recalculate_eye_projections = horizontal_projection_changed || vertical_projection_changed || scale_render_changed;

            ImGui::TreePop();
        }

        ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
        if (ImGui::TreeNode("Splitscreen Compatibility")) {
            m_splitscreen_compatibility_mode->draw("Enabled");
            m_splitscreen_view_index->draw("Index");
            ImGui::TreePop();
        }
    }
    
    if (selected_page == PAGE_DEBUG) {
        if (m_fake_stereo_hook != nullptr) {
            m_fake_stereo_hook->on_draw_ui();
        }

        //ImGui::Combo("Sync Mode", (int*)&get_runtime()->custom_stage, "Early\0Late\0Very Late\0");
        m_sync_mode->draw("Sync Mode");
        ImGui::DragFloat4("Right Bounds", (float*)&m_right_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::DragFloat4("Left Bounds", (float*)&m_left_bounds, 0.005f, -2.0f, 2.0f);
        ImGui::Checkbox("Disable Projection Matrix Override", &m_disable_projection_matrix_override);
        ImGui::Checkbox("Disable View Matrix Override", &m_disable_view_matrix_override);
        ImGui::Checkbox("Disable Backbuffer Size Override", &m_disable_backbuffer_size_override);
        ImGui::Checkbox("Disable VR Overlay", &m_disable_overlay);
        ImGui::Checkbox("Disable VR Entirely", &m_disable_vr);
        ImGui::Checkbox("Stereo Emulation Mode", &m_stereo_emulation_mode);
        ImGui::Checkbox("Wait for Present", &m_wait_for_present);
        m_controllers_allowed->draw("Controllers allowed");
        ImGui::Checkbox("Controller test mode", &m_controller_test_mode);
        m_show_fps->draw("Show FPS");
        m_show_statistics->draw("Show Engine Statistics");
        m_enable_hitch_diagnostics->draw("Enable Hitch Diagnostics");
        if (m_enable_hitch_diagnostics->value()) {
            ImGui::TextWrapped("Records recent OpenXR/D3D12 state and writes hitch_snapshot JSON files after large tick gaps.");
        } else {
            ImGui::TextWrapped("Hitch diagnostics are disabled. No hitch ring sampling, JSON dumps, or snapshot writer thread will run.");
        }

        const double min_ = 0.0;
        const double max_ = 25.0;
        ImGui::SliderScalar("Prediction Scale", ImGuiDataType_Double, &m_openxr->prediction_scale, &min_, &max_);

        ImGui::DragFloat4("Raw Left", (float*)&m_raw_projections[0], 0.01f, -100.0f, 100.0f);
        ImGui::DragFloat4("Raw Right", (float*)&m_raw_projections[1], 0.01f, -100.0f, 100.0f);

        const auto left_stick_axis = get_left_stick_axis();
        const auto right_stick_axis = get_right_stick_axis();

        ImGui::DragFloat2("Left Stick", (float*)&left_stick_axis, 0.01f, -1.0f, 1.0f);
        ImGui::DragFloat2("Right Stick", (float*)&right_stick_axis, 0.01f, -1.0f, 1.0f);

        ImGui::TextWrapped("Hardware scheduling: %s", m_has_hw_scheduling ? "Enabled" : "Disabled");
    }

    ImGui::EndGroup();
    //ImGui::EndTable();
}

void VR::draw_hitch_diagnostics_ui() {
    if (!ImGui::TreeNode("Hitch Diagnostics")) {
        return;
    }

    if (!m_last_hitch_snapshot_summary.valid) {
        ImGui::TextWrapped("No hitch snapshot has been written this session.");
        ImGui::Text("Ring: sequence=%llu cursor=%zu wrapped=%s",
            (unsigned long long)m_hitch_snapshot_sequence,
            m_hitch_snapshot_cursor,
            m_hitch_snapshot_wrapped ? "yes" : "no");
        ImGui::TreePop();
        return;
    }

    const auto& summary = m_last_hitch_snapshot_summary;
    const auto& sample = summary.last_sample;
    const auto& d3d12 = sample.d3d12;

    ImGui::Text("Last snapshot: %s", summary.path.filename().string().c_str());
    ImGui::Text("Tick gap: %lld ms | suspected: %s | samples: %zu",
        (long long)summary.tick_gap_ms,
        summary.suspected_stall.c_str(),
        summary.sample_count);
    ImGui::Text("OpenXR ages: wait=%lldms begin=%lldms end=%lldms pose=%lldms",
        (long long)sample.xr_wait_age_ms,
        (long long)sample.xr_begin_age_ms,
        (long long)sample.xr_end_age_ms,
        (long long)sample.pose_update_age_ms);
    ImGui::Text("Frame state: synced=%s began=%s ready=%s submitted=%s controllers=%s",
        sample.frame_synced ? "yes" : "no",
        sample.frame_began ? "yes" : "no",
        sample.runtime_ready ? "yes" : "no",
        sample.submitted ? "yes" : "no",
        sample.using_controllers ? "yes" : "no");
    ImGui::Text("Stale pose: skips=%llu suppressed=%llu refresh=%llu/%llu/%llu last_pose_frame=%u",
        (unsigned long long)sample.stale_pose_older_frame_skip_count,
        (unsigned long long)sample.stale_pose_older_frame_skip_suppressed_count,
        (unsigned long long)sample.stale_pose_refresh_attempt_count,
        (unsigned long long)sample.stale_pose_refresh_success_count,
        (unsigned long long)sample.stale_pose_refresh_failed_count,
        sample.last_pose_update_frame_count);
    ImGui::Text("Recoveries: focused=%u forced=%u scene_capture_destroyed=%llu",
        sample.focused_frame_loop_recovery_count,
        sample.forced_frame_recovery_count,
        (unsigned long long)sample.scene_capture_destroyed_count);
    ImGui::Text("D3D12: game=%s ui=%s scene_capture=%s recreates=%llu reason=0x%x",
        d3d12.has_game_tex ? "yes" : "no",
        d3d12.has_ui_tex ? "yes" : "no",
        d3d12.has_scene_capture_tex ? "yes" : "no",
        (unsigned long long)d3d12.swapchain_recreate_count,
        d3d12.last_swapchain_recreate_reasons);
    ImGui::Text("D3D12 perf: on_frame avg=%.2f max=%.2f | ui_copy avg=%.2f max=%.2f | submit avg=%.2f max=%.2f",
        d3d12.perf_on_frame_avg_ms,
        d3d12.perf_on_frame_max_ms,
        d3d12.perf_ui_copy_avg_ms,
        d3d12.perf_ui_copy_max_ms,
        d3d12.perf_openxr_submit_avg_ms,
        d3d12.perf_openxr_submit_max_ms);
    ImGui::TextWrapped("Summary file: hitch_snapshots\\hitch_snapshot_latest_summary.json");
    ImGui::TreePop();
}

void VR::on_draw_ui() {
    ZoneScopedN(__FUNCTION__);

    // create VR tree entry in menu (imgui)
    ImGui::PushID("VR");
    ImGui::SetNextItemOpen(true, ImGuiCond_::ImGuiCond_Once);
    if (!m_fake_stereo_hook->has_attempted_to_hook_engine() || !m_fake_stereo_hook->has_attempted_to_hook_slate()) {
        std::string adjusted_name = get_name().data();
        adjusted_name += " (Loading...)";

        /*if (!ImGui::CollapsingHeader(adjusted_name.data())) {
            ImGui::PopID();
            return;
        }*/

        ImGui::TextWrapped("Loading...");
    } else {
        /*if (!ImGui::CollapsingHeader(get_name().data())) {
            ImGui::PopID();
            return;
        }*/
    }
    ImGui::PopID();

    auto display_error = [](auto& runtime, std::string dll_name) {
        if (runtime == nullptr || !runtime->error && runtime->loaded) {
            return;
        }

        if (runtime->error && runtime->dll_missing) {
            ImGui::TextWrapped("%s not loaded: %s not found", runtime->name().data(), dll_name.data());
            ImGui::TextWrapped("Please select %s from the loader if you want to use %s", runtime->name().data(), runtime->name().data());
        } else if (runtime->error) {
            ImGui::TextWrapped("%s not loaded: %s", runtime->name().data(), runtime->error->c_str());
        } else {
            ImGui::TextWrapped("%s not loaded: Unknown error", runtime->name().data());
        }

        ImGui::Separator();
    };

    if (!get_runtime()->loaded || get_runtime()->error) {
        display_error(m_openxr, "openxr_loader.dll");
        display_error(m_openvr, "openvr_api.dll");
    }

    if (!get_runtime()->loaded) {
        ImGui::TextWrapped("No runtime loaded.");

        if (ImGui::Button("Attempt to reinitialize")) {
            clean_initialize();
        }

        return;
    }

    if (ImGui::Button("Set Standing Height")) {
        m_standing_origin.y = get_position(0).y;
    }

    ImGui::SameLine();

    if (ImGui::Button("Set Standing Origin")) {
        m_standing_origin = get_position(0);
    }

    ImGui::SameLine();

    if (ImGui::Button("Recenter View")) {
        recenter_view();
    }

    ImGui::SameLine();

     if (ImGui::Button("Recenter Horizon")) {
        recenter_horizon();
    }
	
    if (ImGui::Button("Reinitialize Runtime")) {
        get_runtime()->wants_reinitialize = true;
    }

    draw_hitch_diagnostics_ui();
}

Vector4f VR::get_position(uint32_t index, bool grip) const {
    return get_transform(index, grip)[3];
}

Vector4f VR::get_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_velocity_unsafe(index);
}

Vector4f VR::get_angular_velocity(uint32_t index) const {
    if (index >= vr::k_unMaxTrackedDeviceCount) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->pose_mtx };

    return get_angular_velocity_unsafe(index);
}

Vector4f VR::get_position_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            auto result = glm::rowMajor4(matrix)[3];
            result.w = 1.0f;

            return result;
        }

        if (index == get_left_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::LEFT][3];
        }

        if (index == get_right_controller_index()) {
            return m_openvr->grip_matrices[VRRuntime::Hand::RIGHT][3];
        }

        auto& pose = get_openvr_poses()[index];
        auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        auto result = glm::rowMajor4(matrix)[3];
        result.w = 1.0f;

        return result;
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // HMD position
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            return Vector4f{ *(Vector3f*)&vspl.pose.position, 1.0f };
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::LEFT][3];
            } else if (index == get_right_controller_index()) {
                return m_openxr->grip_matrices[VRRuntime::Hand::RIGHT][3];
            }
        }

        return Vector4f{};
    } 

    return Vector4f{};
}

Vector4f VR::get_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& velocity = pose.vVelocity;

        return Vector4f{ velocity.v[0], velocity.v[1], velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }

        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.linearVelocity, 0.0f };
    }

    return Vector4f{};
}

Vector4f VR::get_angular_velocity_unsafe(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return Vector4f{};
        }

        const auto& pose = get_openvr_poses()[index];
        const auto& angular_velocity = pose.vAngularVelocity;

        return Vector4f{ angular_velocity.v[0], angular_velocity.v[1], angular_velocity.v[2], 0.0f };
    } else if (get_runtime()->is_openxr()) {
        if (index >= 3) {
            return Vector4f{};
        }

        // todo: implement HMD velocity
        if (index == 0) {
            return Vector4f{};
        }
    
        return Vector4f{ *(Vector3f*)&m_openxr->hands[index-1].grip_velocity.angularVelocity, 0.0f };
    }

    return Vector4f{};
}

Matrix4x4f VR::get_hmd_rotation(uint32_t frame_count) const {
    return glm::extractMatrixRotation(get_hmd_transform(frame_count));
}

Matrix4x4f VR::get_hmd_transform(uint32_t frame_count) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        std::shared_lock _{ get_runtime()->pose_mtx };

        const auto pose = m_openvr->get_hmd_pose(frame_count);
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        std::shared_lock __{ get_runtime()->eyes_mtx };

        const auto vspl = m_openxr->get_view_space_location(frame_count);
        auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
        mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};

        return mat;
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_rotation(uint32_t index, bool grip) const {
    return glm::extractMatrixRotation(get_transform(index, grip));
}

Matrix4x4f VR::get_transform(uint32_t index, bool grip) const {
    ZoneScopedN(__FUNCTION__);

    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return glm::identity<Matrix4x4f>();
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            const auto pose = m_openvr->get_current_hmd_pose();
            const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose };
            return glm::rowMajor4(matrix);
        }

        if (index == get_left_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::LEFT] : m_openvr->aim_matrices[VRRuntime::Hand::LEFT];
        } else if (index == get_right_controller_index()) {
            return grip ? m_openvr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openvr->aim_matrices[VRRuntime::Hand::RIGHT];
        }

        const auto& pose = get_openvr_poses()[index];
        const auto matrix = Matrix4x4f{ *(Matrix3x4f*)&pose.mDeviceToAbsoluteTracking };
        return glm::rowMajor4(matrix);
    } else if (get_runtime()->is_openxr()) {
        // HMD rotation
        if (index == 0 && !m_openxr->stage_views.empty()) {
            const auto vspl = m_openxr->get_current_view_space_location();
            auto mat = Matrix4x4f{runtimes::OpenXR::to_glm(vspl.pose.orientation)};
            mat[3] = Vector4f{*(Vector3f*)&vspl.pose.position, 1.0f};
            return mat;
        } else if (index > 0) {
            if (index == get_left_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::LEFT] : m_openxr->aim_matrices[VRRuntime::Hand::LEFT];
            } else if (index == get_right_controller_index()) {
                return grip ? m_openxr->grip_matrices[VRRuntime::Hand::RIGHT] : m_openxr->aim_matrices[VRRuntime::Hand::RIGHT];
            }
        }
    }

    return glm::identity<Matrix4x4f>();
}

Matrix4x4f VR::get_grip_transform(uint32_t index) const {
    return get_transform(index);
}

Matrix4x4f VR::get_aim_transform(uint32_t index) const {
    return get_transform(index, false);
}

vr::HmdMatrix34_t VR::get_raw_transform(uint32_t index) const {
    if (get_runtime()->is_openvr()) {
        if (index >= vr::k_unMaxTrackedDeviceCount) {
            return vr::HmdMatrix34_t{};
        }

        std::shared_lock _{ get_runtime()->pose_mtx };

        if (index == vr::k_unTrackedDeviceIndex_Hmd) {
            return m_openvr->get_current_hmd_pose();
        }

        auto& pose = get_openvr_poses()[index];
        return pose.mDeviceToAbsoluteTracking;
    } else {
        spdlog::error("VR: get_raw_transform: not implemented for {}", get_runtime()->name());
        return vr::HmdMatrix34_t{};
    }
}

Vector4f VR::get_eye_offset(VRRuntime::Eye eye) const {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (eye == VRRuntime::Eye::LEFT) {
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
}

Vector4f VR::get_current_offset() {
    if (!is_hmd_active()) {
        return Vector4f{};
    }

    std::shared_lock _{ get_runtime()->eyes_mtx };

    if (m_frame_count % 2 == m_left_eye_interval) {
        //return Vector4f{m_eye_distance * -1.0f, 0.0f, 0.0f, 0.0f};
        return get_runtime()->eyes[vr::Eye_Left][3];
    }
    
    return get_runtime()->eyes[vr::Eye_Right][3];
    //return Vector4f{m_eye_distance, 0.0f, 0.0f, 0.0f};
}

Matrix4x4f VR::get_eye_transform(uint32_t index) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active() || index > 2) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    return get_runtime()->eyes[index];
}

Matrix4x4f VR::get_current_eye_transform(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->eyes[vr::Eye_Left];
    }

    return get_runtime()->eyes[vr::Eye_Right];
}

Matrix4x4f VR::get_projection_matrix(VRRuntime::Eye eye, bool flip) {
    ZoneScopedN(__FUNCTION__);

    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto out = ((eye == VRRuntime::Eye::LEFT && !flip) || (eye == VRRuntime::Eye::RIGHT && flip))
        ? get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT]
        : get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];

    if (m_match_game_fov->value() && !m_match_game_fov_dolly->value()) {
        const auto m00 = out[0][0];
        if (std::isfinite(m00) && m00 != 0.0f) {
            const auto base_half_fov = std::atan(1.0f / std::abs(m00));
            const auto scale = get_game_fov_scale(base_half_fov);
            if (scale != 1.0f) {
                out[0][0] *= scale;
                out[1][1] *= scale;
            }
        }
    }

    return out;
}

Matrix4x4f VR::get_current_projection_matrix(bool flip) {
    if (!is_hmd_active()) {
        return glm::identity<Matrix4x4f>();
    }

    std::shared_lock _{get_runtime()->eyes_mtx};

    auto mod_count = flip ? m_right_eye_interval : m_left_eye_interval;

    if (m_frame_count % 2 == mod_count) {
        return get_runtime()->projections[(uint32_t)VRRuntime::Eye::LEFT];
    }

    return get_runtime()->projections[(uint32_t)VRRuntime::Eye::RIGHT];
}

bool VR::is_action_active(vr::VRActionHandle_t action, vr::VRInputValueHandle_t source) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return false;
    }

    if (action == vr::k_ulInvalidActionHandle) {
        return false;
    }
    
    bool active = false;

    if (get_runtime()->is_openvr()) {
        vr::InputDigitalActionData_t data{};
        vr::VRInput()->GetDigitalActionData(action, &data, sizeof(data), source);

        active = data.bActive && data.bState;
    } else if (get_runtime()->is_openxr()) {
        active = m_openxr->is_action_active((XrAction)action, (VRRuntime::Hand)source);
    }

    return active;
}

Vector2f VR::get_joystick_axis(vr::VRInputValueHandle_t handle) const {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded) {
        return Vector2f{};
    }

    if (get_runtime()->is_openvr()) {
        vr::InputAnalogActionData_t data{};
        vr::VRInput()->GetAnalogActionData(m_action_joystick, &data, sizeof(data), handle);

        const auto deadzone = m_joystick_deadzone->value();
        auto out = Vector2f{ data.x, data.y };

        //return glm::length(out) > deadzone ? out : Vector2f{};
        if (glm::abs(out.x) < deadzone) {
            out.x = 0.0f;
        }

        if (glm::abs(out.y) < deadzone) {
            out.y = 0.0f;
        }

        return out;
    } else if (get_runtime()->is_openxr()) {
        // Not using get_left/right_joystick here because it flips the controllers
        if (handle == m_left_joystick) {
            auto out = m_openxr->get_left_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};
            // okay.. instead of that actually clamp x/y to the proper deadzone
            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        } else if (handle == m_right_joystick) {
            auto out = m_openxr->get_right_stick_axis();
            //return glm::length(out) > m_joystick_deadzone->value() ? out : Vector2f{};

            if (glm::abs(out.x) < m_joystick_deadzone->value()) {
                out.x = 0.0f;
            }

            if (glm::abs(out.y) < m_joystick_deadzone->value()) {
                out.y = 0.0f;
            }

            return out;
        }
    }

    return Vector2f{};
}

Vector2f VR::get_left_stick_axis() const {
    return get_joystick_axis(get_left_joystick());
}

Vector2f VR::get_right_stick_axis() const {
    return get_joystick_axis(get_right_joystick());
}

void VR::trigger_haptic_vibration(float seconds_from_now, float duration, float frequency, float amplitude, vr::VRInputValueHandle_t source) {
    ZoneScopedN(__FUNCTION__);

    if (!get_runtime()->loaded || !is_using_controllers()) {
        return;
    }

    if (get_runtime()->is_openvr()) {
        vr::VRInput()->TriggerHapticVibrationAction(m_action_haptic, seconds_from_now, duration, frequency, amplitude, source);
    } else if (get_runtime()->is_openxr()) {
        m_openxr->trigger_haptic_vibration(duration, frequency, amplitude, (VRRuntime::Hand)source);
    }
}

float VR::get_standing_height() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin.y;
}

Vector4f VR::get_standing_origin() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ get_runtime()->pose_mtx };

    return m_standing_origin;
}

void VR::set_standing_origin(const Vector4f& origin) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ get_runtime()->pose_mtx };
    
    m_standing_origin = origin;
}

glm::quat VR::get_rotation_offset() {
    ZoneScopedN(__FUNCTION__);

    std::shared_lock _{ m_rotation_mtx };

    return m_rotation_offset;
}

void VR::set_rotation_offset(const glm::quat& offset) {
    ZoneScopedN(__FUNCTION__);

    std::unique_lock _{ m_rotation_mtx };

    m_rotation_offset = offset;
}

void VR::recenter_view() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(utility::math::flatten(glm::quat{get_rotation(0)})));

    set_rotation_offset(new_rotation_offset);
}

void VR::recenter_horizon() {
    ZoneScopedN(__FUNCTION__);

    const auto new_rotation_offset = glm::normalize(glm::inverse(glm::quat{get_rotation(0)}));

    set_rotation_offset(new_rotation_offset);
}

void VR::gamepad_snapturn(XINPUT_STATE& state) {
    if (!m_snapturn->value()) {
        return;
    }

    if (!is_hmd_active()) {
        return;
    }

    const auto stick_axis = (float)state.Gamepad.sThumbRX / (float)std::numeric_limits<SHORT>::max();

    if (!m_was_snapturn_run_on_input) {
        if (glm::abs(stick_axis) > m_snapturn_joystick_deadzone->value()) {
            m_snapturn_left = stick_axis < 0.0f;
            m_snapturn_on_frame = true;
            m_was_snapturn_run_on_input = true;
            state.Gamepad.sThumbRX = 0;
        }
    } else {
        if (glm::abs(stick_axis) < m_snapturn_joystick_deadzone->value()) {
            m_was_snapturn_run_on_input = false;
        } else {
            state.Gamepad.sThumbRX = 0;
        }
    }
}

void VR::process_snapturn() {
    if (!m_snapturn_on_frame) {
        return;
    }

    const auto engine = sdk::UEngine::get();

    if (engine == nullptr) {
        return;
    }

    const auto world = engine->get_world();

    if (world == nullptr) {
        return;
    }

    if (const auto controller = sdk::UGameplayStatics::get()->get_player_controller(world, 0); controller != nullptr) {
        auto controller_rot = controller->get_control_rotation();
        auto turn_degrees = get_snapturn_angle();
        
        if (m_snapturn_left) {
            turn_degrees = -turn_degrees;
            m_snapturn_left = false;
        }

        controller_rot.y += turn_degrees;
        controller->set_control_rotation(controller_rot);
    }
        
    m_snapturn_on_frame = false;

}

void VR::update_statistics_overlay(sdk::UGameEngine* engine) {
    if (engine == nullptr) {
        return;
    }

    if (m_show_fps_state != m_show_fps->value()) {
        engine->exec(L"stat fps");
        m_show_fps_state = m_show_fps->value();
    }

    if (m_show_statistics_state != m_show_statistics->value()) {
        engine->exec(L"stat unit");
        m_show_statistics_state = m_show_statistics->value();
    }
}
