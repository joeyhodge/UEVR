#include "../VR.hpp"

#include <fstream>
#include <imgui.h>
#include <Windows.h>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include "Framework.hpp"
#include "utility/BuildIdentity.hpp"
#include "utility/PacingRecorderJson.hpp"
#include "utility/SupportHealth.hpp"

namespace {
const char* rendering_method_name(int value) {
    switch (value) {
    case VR::NATIVE_STEREO: return "Native Stereo";
    case VR::SYNCHRONIZED: return "Synchronized Sequential";
    case VR::ALTERNATING: return "Alternating / AFR";
    case VR::SYNTHETIC_DIBR: return "DIBR";
    case VR::SYNTHETIC_DIBR_SINGLE_VIEW: return "DIBR Single View";
    case VR::MONO: return "Mono";
    default: return "Unknown / other experiment";
    }
}
uint64_t file_time(FILETIME value) {
    return uint64_t(value.dwHighDateTime) << 32 | value.dwLowDateTime;
}
nlohmann::json process_identity() {
    const auto executable = utility::get_module_pathw(utility::get_executable());
    HMODULE backend{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&utility::support::current_build_identity), &backend);
    const auto backend_path = backend ? utility::get_module_pathw(backend) : std::nullopt;
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool have_time = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
    return {{"pid", GetCurrentProcessId()},
        {"start_filetime", have_time ? nlohmann::json(std::to_string(file_time(created))) : nlohmann::json(nullptr)},
        {"executable", executable ? nlohmann::json(utility::narrow(*executable)) : nlohmann::json(nullptr)},
        {"backend_path", backend_path ? nlohmann::json(utility::narrow(*backend_path)) : nlohmann::json(nullptr)}};
}
nlohmann::json file_resource_version() {
    // This is file metadata, not engine/ABI discovery. Some games use an app version here.
    const auto path = utility::get_module_pathw(utility::get_executable());
    if (!path) { return nullptr; }
    DWORD unused{};
    const auto size = GetFileVersionInfoSizeW(path->c_str(), &unused);
    if (!size || size > 1024 * 1024) { return nullptr; }
    std::vector<uint8_t> bytes(size);
    VS_FIXEDFILEINFO* info{};
    UINT length{};
    if (!GetFileVersionInfoW(path->c_str(), 0, size, bytes.data()) ||
        !VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(*info) || info->dwSignature != 0xFEEF04BD) { return nullptr; }
    return fmt::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
        HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}
}

nlohmann::json VR::get_health_snapshot() {
    LARGE_INTEGER qpc_before{}, qpc_after{}, qpc_frequency{};
    const bool have_qpc = QueryPerformanceFrequency(&qpc_frequency) && QueryPerformanceCounter(&qpc_before);
    const auto steady_sample = utility::pacing::now_ns();
    const bool have_qpc_after = QueryPerformanceCounter(&qpc_after) != FALSE;
    auto support = get_support_diagnostics();
    support["features"] = {
        {"native_stereo", is_using_native_stereo()}, {"native_fix_requested", m_native_stereo_fix->value()},
        {"native_fix_status", m_fake_stereo_hook ? m_fake_stereo_hook->get_native_stereo_fix_status_text() : "hooks unavailable"},
        {"native_fix_operational", m_fake_stereo_hook && m_fake_stereo_hook->is_native_stereo_fix_operational()},
        {"same_pass", m_native_stereo_fix_same_pass->value()},
        {"array_requested", m_native_stereo_fix_texture_array_submit->value()},
        {"preacquire_requested", m_native_stereo_fix_async_openxr_wait->value()},
        {"wait_only_requested", m_native_openxr_wait_only->value()},
        {"ghost_fix_requested", m_ghosting_fix->value()},
        {"ghost_fix_status", m_fake_stereo_hook ? m_fake_stereo_hook->try_get_ghosting_fix_status_text() : "hooks unavailable"},
        {"hitch_diagnostics", m_enable_hitch_diagnostics->value()},
        {"camera_trace", nullptr},
        {"pacing_recorder", utility::pacing::recorder().enabled()},
        {"ui_automatic_capability", m_fake_stereo_hook
            ? nlohmann::json(uevr::vr_compatibility::to_string(m_fake_stereo_hook->get_ue58_dedicated_ui_capability())) : nlohmann::json(nullptr)},
        {"pre57_ui_observation", m_fake_stereo_hook ? m_fake_stereo_hook->try_get_pre57_slate_ui_observation_status() : "hooks unavailable"},
    };
    auto runtime = nlohmann::json{{"sampled", false}};
    {
        std::unique_lock lifetime{m_openvr_mtx, std::try_to_lock};
        if (lifetime.owns_lock() && m_runtime) {
            runtime = {{"sampled", true}, {"name", m_runtime->name()}, {"loaded", m_runtime->loaded},
                {"diagnostic_id", m_runtime->diagnostic_id}};
        }
    }
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto result = nlohmann::json{{"schema_version", 1}, {"diagnostic_only", true}, {"snapshot_unix_ms", stamp},
        {"build", utility::support::current_build_identity()}, {"process", process_identity()},
        {"profile_dir", Framework::get_persistent_dir("support_snapshot.json").parent_path().string()},
        {"executable_file_version", file_resource_version()},
        {"engine_version_note", "File version is not ABI authority. Use existing Hook Provenance for detected engine/layout proofs; this view initiates no discovery."},
        {"rhi", g_framework->is_dx12() ? "D3D12" : g_framework->is_dx11() ? "D3D11" : "unavailable"},
        {"branch_capabilities", {{"mono", true}, {"dibr", true}, {"prospi_camera_trace", false}}},
        {"runtime", std::move(runtime)}, {"support", support},
        {"clock_correlation", {{"available", have_qpc && have_qpc_after}, {"steady_ns", steady_sample},
            {"qpc_before", qpc_before.QuadPart}, {"qpc_after", qpc_after.QuadPart}, {"qpc_frequency", qpc_frequency.QuadPart}}},
        {"findings", utility::support::health_findings(support)}};
    return result;
}

void VR::draw_health_diagnostics() {
    namespace pacing = utility::pacing;
    ImGui::TextWrapped("Read-only health and pacing observations. No options are automatically changed and no engine scans are started.");
    bool recording = pacing::recorder().enabled();
    if (ImGui::Checkbox("Record Frame Pacing (session only)", &recording)) {
        pacing::recorder().set_enabled(recording);
    }
    ImGui::TextWrapped("Off by default on every launch. Stop to retain history; re-enable to start fresh. CPU/API intervals only, not GPU timings. No per-frame logs, file writes or waiting for telemetry.");
    pacing::Recorder<>::Summary summary{};
    if (pacing::recorder().summary(summary)) {
        ImGui::Text("Retained: %llu / 4096; overwritten: %llu; dropped (lifetime): %llu",
            static_cast<unsigned long long>(summary.retained), static_cast<unsigned long long>(summary.overwritten),
            static_cast<unsigned long long>(summary.dropped_lifetime));
    } else { ImGui::TextDisabled("Recorder snapshot busy; rendering is not blocked."); }
    ImGui::BeginDisabled(!recording);
    if (ImGui::Button("Mark Current Frame")) { pacing::ScopedEvent marker{pacing::Stage::marker}; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool refresh = ImGui::Button("Refresh Health");
    if (refresh) { m_cvar_manager->request_diagnostic_snapshot(); }
    const auto cvar_revision = m_cvar_manager->diagnostic_snapshot_revision();
    if (refresh || m_health_snapshot.is_null() ||
        m_health_cvar_revision != cvar_revision) {
        try {
            m_health_snapshot = get_health_snapshot();
            m_health_snapshot_text = m_health_snapshot.dump(2);
            m_health_cvar_revision = cvar_revision;
        } catch (const std::exception& error) {
            m_health_export_status = fmt::format("Health snapshot unavailable: {}", error.what());
        }
    }
    ImGui::TextWrapped("Refresh samples already-resolved CVars on the next game tick. Status below is the last requested snapshot, not continuous monitoring.");
    if (m_health_snapshot.is_object()) {
        const auto& build = m_health_snapshot["build"];
        ImGui::TextWrapped("Backend: %s; SDK: %s; tracked edits: %s / %s",
            build.value("uevr_commit", "unavailable").c_str(), build.value("uesdk_commit", "unavailable").c_str(),
            build.value("uevr_tracked_changes", "unavailable").c_str(), build.value("uesdk_tracked_changes", "unavailable").c_str());
        const auto& support = m_health_snapshot["support"];
        const auto& features = support["features"];
        const auto& method = support["rendering_method"];
        ImGui::TextWrapped("Mode: %s -> %s; runtime: %s",
            rendering_method_name(method.value("requested", -1)), rendering_method_name(method.value("effective", -1)),
            m_health_snapshot["runtime"].value("name", "snapshot unavailable").c_str());
        if (m_health_snapshot["executable_file_version"].is_string()) {
            ImGui::TextWrapped("EXE file version: %s (metadata only, not engine ABI authority)",
                m_health_snapshot["executable_file_version"].get_ref<const std::string&>().c_str());
        }
        ImGui::TextWrapped("RHI: %s; Native Fix: %s", m_health_snapshot.value("rhi", "unavailable").c_str(),
            features.value("native_fix_status", "unavailable").c_str());
        ImGui::TextWrapped("Ghost Fix: %s; automatic UI capability: %s", features.value("ghost_fix_status", "unavailable").c_str(),
            features.value("ui_automatic_capability", "unavailable").c_str());
        ImGui::TextWrapped("Pre-5.7 UI observation: %s (observation status, not proof of correct UI pixels)",
            features.value("pre57_ui_observation", "unavailable").c_str());
        if (support["openxr"].contains("native_wait_only")) {
            const auto& wait = support["openxr"]["native_wait_only"];
            ImGui::TextWrapped("Wait-only: %s; completed waits worker/consumer: %llu / %llu", wait.value("status", "unavailable").c_str(),
                static_cast<unsigned long long>(wait.value("worker_waits", uint64_t{})),
                static_cast<unsigned long long>(wait.value("cooperative_waits", uint64_t{})));
        }
        for (const auto& finding : m_health_snapshot["findings"]) {
            ImGui::BulletText("%s", finding["message"].get_ref<const std::string&>().c_str());
        }
    }
    if (ImGui::Button("Copy Health JSON")) { ImGui::SetClipboardText(m_health_snapshot_text.c_str()); }
    ImGui::SameLine();
    if (ImGui::Button("Export Support Snapshot")) {
        try {
            auto snapshot = get_health_snapshot();
            snapshot["frame_pacing"] = pacing::export_json(pacing::recorder());
            const auto text = snapshot.dump(2);
            const auto path = Framework::get_persistent_dir("support_snapshot.json");
            const auto temporary = Framework::get_persistent_dir("support_snapshot.json.tmp");
            std::ofstream output{temporary, std::ios::binary | std::ios::trunc};
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            output.flush();
            if (!output.good()) { throw std::runtime_error{"Could not write the support snapshot"}; }
            output.close();
            if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                throw std::runtime_error{"Could not publish the complete support snapshot"};
            }
            m_health_export_status = "Exported " + path.string() + ". Use support/capture.cmd beside the injector (tools/support in source) for a build-verified bundle/optional CPU trace. Review before sharing; paths and settings may be included.";
        } catch (const std::exception& error) {
            m_health_export_status = fmt::format("Support export failed: {}", error.what());
        }
    }
    if (!m_health_export_status.empty()) { ImGui::TextWrapped("%s", m_health_export_status.c_str()); }
    if (ImGui::TreeNode("Full Health Snapshot")) {
        ImGui::TextUnformatted(m_health_snapshot_text.c_str());
        ImGui::TreePop();
    }
}
