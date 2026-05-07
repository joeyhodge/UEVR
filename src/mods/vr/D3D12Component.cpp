#include <d3dcompiler.h>

#include <openvr.h>
#include <utility/Module.hpp>
#include <utility/String.hpp>
#include <utility/ScopeGuard.hpp>
#include <utility/Logging.hpp>
#include <algorithm>
#include <array>
#include <DirectXMath.h>
#include <mutex>
#include <sstream>
#include <unordered_set>

#include "Framework.hpp"
#include "render/D3D12Diagnostics.hpp"
#include "../GameSpecific.hpp"
#include "../VR.hpp"

#include <sdk/Utility.hpp>

#include <../../directxtk12-src/Inc/ResourceUploadBatch.h>
#include <../../directxtk12-src/Inc/RenderTargetState.h>

#include "shaders/Compiled/alpha_luminance_sprite_ps_SpritePixelShader.inc"
#include "shaders/Compiled/alpha_luminance_sprite_ps_SpriteVertexShader.inc"

#include "d3d12/DirectXTK.hpp"

#include "D3D12Component.hpp"

//#define AFR_DEPTH_TEMP_DISABLED

constexpr auto ENGINE_SRC_DEPTH = D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr auto ENGINE_SRC_COLOR = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

namespace vrmod {
namespace {
constexpr auto FRAME_TIMING_LOG_INTERVAL = std::chrono::seconds(5);
constexpr bool SHF_AUTO_MONO_CINEMATIC = true;
constexpr bool SHF_AUTO_2D_SCREEN_FROM_MONO_CINEMATIC = true;

enum SwapchainRecreateReason : uint32_t {
    SWAPCHAIN_RECREATE_NONE = 0,
    SWAPCHAIN_RECREATE_HMD_RESOLUTION = 1 << 0,
    SWAPCHAIN_RECREATE_EMPTY = 1 << 1,
    SWAPCHAIN_RECREATE_UI_EXTENT = 1 << 2,
    SWAPCHAIN_RECREATE_AFR_STATE = 1 << 3,
    SWAPCHAIN_RECREATE_DEPTH_EXTENT = 1 << 4,
    SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS = 1 << 5,
};

std::string format_swapchain_recreate_reasons(uint32_t reasons) {
    if (reasons == SWAPCHAIN_RECREATE_NONE) {
        return "none";
    }

    std::string out{};
    const auto append = [&](uint32_t flag, const char* name) {
        if ((reasons & flag) == 0) {
            return;
        }

        if (!out.empty()) {
            out += "|";
        }

        out += name;
    };

    append(SWAPCHAIN_RECREATE_HMD_RESOLUTION, "hmd_resolution");
    append(SWAPCHAIN_RECREATE_EMPTY, "empty_swapchains");
    append(SWAPCHAIN_RECREATE_UI_EXTENT, "ui_extent");
    append(SWAPCHAIN_RECREATE_AFR_STATE, "afr_state");
    append(SWAPCHAIN_RECREATE_DEPTH_EXTENT, "depth_extent");
    append(SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS, "depth_null_defaults");
    return out;
}

void prepare_openxr_swapchain_recreate(VR* vr, uint32_t reasons) {
    const auto cadence_sensitive_recreate =
        (reasons & (SWAPCHAIN_RECREATE_AFR_STATE | SWAPCHAIN_RECREATE_DEPTH_EXTENT | SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS)) != 0;

    if (!cadence_sensitive_recreate) {
        return;
    }

    if (vr == nullptr || vr->get_runtime() == nullptr || !vr->get_runtime()->is_openxr()) {
        return;
    }

    const auto openxr = vr->get_openxr_runtime();

    if (openxr == nullptr) {
        return;
    }

    const auto reason_text = "d3d12_swapchain_recreate:" + format_swapchain_recreate_reasons(reasons);
    openxr->prepare_resolution_scale_reconfigure(reason_text.c_str());
}

std::pair<uint32_t, uint32_t> get_ui_extent() {
    const auto fallback = std::pair<uint32_t, uint32_t>{
        (uint32_t)g_framework->get_d3d12_rt_size().x,
        (uint32_t)g_framework->get_d3d12_rt_size().y
    };

    const auto vr = VR::get();

    if (vr == nullptr) {
        return fallback;
    }

    const auto& fake_stereo_hook = vr->get_fake_stereo_hook();

    if (fake_stereo_hook == nullptr) {
        return fallback;
    }

    const auto rtm = fake_stereo_hook->get_render_target_manager();

    if (rtm == nullptr) {
        return fallback;
    }

    if (const auto requested_width = rtm->get_dedicated_ui_width();
        requested_width > 0 && rtm->get_dedicated_ui_height() > 0)
    {
        return {requested_width, rtm->get_dedicated_ui_height()};
    }

    const auto ui_target = rtm->get_ui_target();

    if (ui_target == nullptr || !g_framework->is_dx12()) {
        return fallback;
    }

    const auto native = (ID3D12Resource*)ui_target->get_native_resource();

    if (native == nullptr) {
        return fallback;
    }

    const auto desc = native->GetDesc();

    if (desc.Width == 0 || desc.Height == 0) {
        return fallback;
    }

    return {(uint32_t)desc.Width, (uint32_t)desc.Height};
}

const char* openxr_swapchain_name(uint32_t index) {
    switch (static_cast<runtimes::OpenXR::SwapchainIndex>(index)) {
    case runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE:
        return "DOUBLE_WIDE";
    case runtimes::OpenXR::SwapchainIndex::DEPTH:
        return "DEPTH";
    case runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP:
        return "DUMMY_VIRTUAL_DESKTOP";
    case runtimes::OpenXR::SwapchainIndex::UI:
        return "UI";
    case runtimes::OpenXR::SwapchainIndex::UI_RIGHT:
        return "UI_RIGHT";
    case runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI:
        return "FRAMEWORK_UI";
    case runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE:
        return "AFR_LEFT_EYE";
    case runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE:
        return "AFR_RIGHT_EYE";
    case runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE:
        return "AFR_DEPTH_LEFT_EYE";
    case runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE:
        return "AFR_DEPTH_RIGHT_EYE";
    default:
        return "UNKNOWN";
    }
}

bool is_depth_swapchain(uint32_t index) {
    const auto swapchain = static_cast<runtimes::OpenXR::SwapchainIndex>(index);
    return swapchain == runtimes::OpenXR::SwapchainIndex::DEPTH ||
        swapchain == runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE ||
        swapchain == runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE;
}

bool is_ui_swapchain(uint32_t index) {
    const auto swapchain = static_cast<runtimes::OpenXR::SwapchainIndex>(index);
    return swapchain == runtimes::OpenXR::SwapchainIndex::UI ||
        swapchain == runtimes::OpenXR::SwapchainIndex::UI_RIGHT ||
        swapchain == runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI;
}

bool is_scene_swapchain(uint32_t index) {
    const auto swapchain = static_cast<runtimes::OpenXR::SwapchainIndex>(index);
    return swapchain == runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE ||
        swapchain == runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE ||
        swapchain == runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE;
}

bool is_shf_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"SHf-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_stalker2_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && exe_path->find(L"Stalker2-Win64-Shipping") != std::wstring::npos;
    }();

    return result;
}

bool is_avowed_current_game() {
    static const bool result = []() {
        const auto exe_path = utility::get_module_pathw(utility::get_executable());
        return exe_path && uevr::games::is_avowed_executable_path(*exe_path);
    }();

    return result;
}

bool is_ue_5_1_dx12_backend() {
    if (g_framework == nullptr || !g_framework->is_dx12()) {
        return false;
    }

    static const bool result = []() {
        const auto found_version = sdk::search_for_version(utility::get_executable());

        if (found_version) {
            const auto version = utility::narrow(*found_version);
            return version == "5.1" || version.starts_with("5.1.");
        }

        const auto disk_version = sdk::get_file_version_info();
        return disk_version.dwFileVersionMS == 0x00050001;
    }();

    return result;
}

bool texture_context_has_views(const d3d12::TextureContext& context) {
    return context.texture.Get() != nullptr &&
        context.rtv_heap != nullptr &&
        context.rtv_heap->Heap() != nullptr &&
        context.srv_heap != nullptr &&
        context.srv_heap->Heap() != nullptr;
}

void log_shf_texture_reference_rebuild(
    ID3D12Resource* backbuffer,
    ID3D12Resource* real_backbuffer,
    ID3D12Resource* current_game_texture,
    uint64_t frame_count)
{
    if (!is_shf_current_game() || backbuffer == nullptr) {
        return;
    }

    const auto backbuffer_desc = backbuffer->GetDesc();
    const auto real_desc = real_backbuffer != nullptr ? std::optional<D3D12_RESOURCE_DESC>{real_backbuffer->GetDesc()} : std::nullopt;
    static std::mutex log_mutex{};
    static std::unordered_set<uintptr_t> logged_backbuffers{};
    static uint64_t rebuild_count{};
    static uint64_t duplicate_suppressed{};

    bool log_unique = false;
    uint64_t seen = 0;
    uint64_t unique = 0;
    uint64_t suppressed = 0;

    {
        std::scoped_lock _{log_mutex};
        ++rebuild_count;
        seen = rebuild_count;

        const auto key = (uintptr_t)backbuffer;

        if (!logged_backbuffers.contains(key)) {
            logged_backbuffers.insert(key);
            log_unique = logged_backbuffers.size() <= 64;
        } else {
            ++duplicate_suppressed;
        }

        unique = logged_backbuffers.size();
        suppressed = duplicate_suppressed;
    }

    if (log_unique && real_desc) {
        SPDLOG_WARN("[SHf][D3D12] Game Texture reference rebuild #{} frame={} unique_backbuffers={} backbuffer={:x} real_backbuffer={:x} current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}]",
            seen, frame_count, unique, (uintptr_t)backbuffer, (uintptr_t)real_backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags,
            real_desc->Width, real_desc->Height, (uint32_t)real_desc->Format, (uint32_t)real_desc->Flags);
    } else if (log_unique) {
        SPDLOG_WARN("[SHf][D3D12] Game Texture reference rebuild #{} frame={} unique_backbuffers={} backbuffer={:x} real_backbuffer=<null> current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}]",
            seen, frame_count, unique, (uintptr_t)backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags);
    } else if (real_desc) {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][D3D12] Game Texture reference rebuild summary seen={} unique_backbuffers={} duplicate_suppressed={} frame={} backbuffer={:x} real_backbuffer={:x} current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}]",
            seen, unique, suppressed, frame_count, (uintptr_t)backbuffer, (uintptr_t)real_backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags,
            real_desc->Width, real_desc->Height, (uint32_t)real_desc->Format, (uint32_t)real_desc->Flags);
    } else {
        SPDLOG_INFO_EVERY_N_SEC(2,
            "[SHf][D3D12] Game Texture reference rebuild summary seen={} unique_backbuffers={} duplicate_suppressed={} frame={} backbuffer={:x} real_backbuffer=<null> current_game_texture={:x} bb=[{}x{} fmt={} flags=0x{:x}]",
            seen, unique, suppressed, frame_count, (uintptr_t)backbuffer, (uintptr_t)current_game_texture,
            backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format, (uint32_t)backbuffer_desc.Flags);
    }
}

bool shf_texture_desc_matches(const D3D12_RESOURCE_DESC& a, const D3D12_RESOURCE_DESC& b) {
    return a.Dimension == b.Dimension &&
           a.Alignment == b.Alignment &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

}

const char* D3D12Component::shf_scene_mode_name(ShfSceneMode mode) {
    switch (mode) {
    case ShfSceneMode::Stereo3D:
        return "Stereo3D";
    case ShfSceneMode::Mono2D:
        return "Mono2D";
    default:
        return "Unknown";
    }
}

bool D3D12Component::ensure_2d_screen_textures(ID3D12Device* device, const D3D12_RESOURCE_DESC& base_desc) {
    if (device == nullptr) {
        return false;
    }

    auto screen_desc = base_desc;
    screen_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    screen_desc.Alignment = 0;
    screen_desc.Width = (uint32_t)g_framework->get_d3d12_rt_size().x;
    screen_desc.Height = (uint32_t)g_framework->get_d3d12_rt_size().y;
    screen_desc.DepthOrArraySize = 1;
    screen_desc.MipLevels = 1;
    screen_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    screen_desc.SampleDesc.Count = 1;
    screen_desc.SampleDesc.Quality = 0;
    screen_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    screen_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    screen_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (screen_desc.Width == 0 || screen_desc.Height == 0) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Refusing to create zero-sized 2D screen textures (D3D12).");
        return false;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    bool all_ready = true;

    for (auto& context : m_2d_screen_tex) {
        bool needs_create = context.texture.Get() == nullptr;

        if (!needs_create) {
            const auto existing_desc = context.texture->GetDesc();
            needs_create =
                existing_desc.Width != screen_desc.Width ||
                existing_desc.Height != screen_desc.Height ||
                existing_desc.Format != screen_desc.Format ||
                existing_desc.SampleDesc.Count != screen_desc.SampleDesc.Count ||
                existing_desc.SampleDesc.Quality != screen_desc.SampleDesc.Quality;
        }

        if (!needs_create) {
            continue;
        }

        context.reset();

        ComPtr<ID3D12Resource> screen_tex{};
        if (FAILED(device->CreateCommittedResource(
                &heap_props,
                D3D12_HEAP_FLAG_NONE,
                &screen_desc,
                ENGINE_SRC_COLOR,
                nullptr,
                IID_PPV_ARGS(&screen_tex)))) {
            spdlog::error("[VR] Failed to create 2D screen texture.");
            all_ready = false;
            continue;
        }

        screen_tex->SetName(L"2D Screen Texture");

        if (!context.setup(device, screen_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"2D Screen")) {
            spdlog::error("[VR] Failed to setup 2D screen context.");
            context.reset();
            all_ready = false;
            continue;
        }

        SPDLOG_INFO("[VR] Created D3D12 2D screen texture [{}x{} fmt={}]", screen_desc.Width, screen_desc.Height, (uint32_t)screen_desc.Format);
    }

    return all_ready;
}

D3D12Component::ShfSceneMode D3D12Component::classify_shf_scene_mode(
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_desc) const
{
    const auto source_width = (uint64_t)source_desc.Width;
    const auto source_height = (uint32_t)source_desc.Height;
    const auto real_width = (uint64_t)real_desc.Width;
    const auto real_height = (uint32_t)real_desc.Height;

    if (real_width > 0 && real_height > 0 && source_width == real_width * 2 && source_height == real_height) {
        return ShfSceneMode::Mono2D;
    }

    if (m_backbuffer_size[0] != 0 && m_backbuffer_size[1] != 0 &&
        source_width == m_backbuffer_size[0] && source_height == m_backbuffer_size[1]) {
        return ShfSceneMode::Stereo3D;
    }

    if (source_width > real_width * 2 || source_height > real_height) {
        return ShfSceneMode::Stereo3D;
    }

    return ShfSceneMode::Unknown;
}

void D3D12Component::log_shf_scene_mode_if_needed(
    ShfSceneMode mode,
    const D3D12_RESOURCE_DESC& source_desc,
    const D3D12_RESOURCE_DESC& real_desc,
    uint64_t frame_count,
    bool using_mono_expansion)
{
    if (!is_shf_current_game()) {
        return;
    }

    if (m_shf_scene_mode != mode) {
        SPDLOG_WARN(
            "[SHf][D3D12] Scene mode changed {} -> {} frame={} src=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}] normal_dw={}x{} mono_expanded={}",
            shf_scene_mode_name(m_shf_scene_mode),
            shf_scene_mode_name(mode),
            frame_count,
            source_desc.Width,
            source_desc.Height,
            (uint32_t)source_desc.Format,
            (uint32_t)source_desc.Flags,
            real_desc.Width,
            real_desc.Height,
            (uint32_t)real_desc.Format,
            (uint32_t)real_desc.Flags,
            m_backbuffer_size[0],
            m_backbuffer_size[1],
            using_mono_expansion);
        m_shf_scene_mode = mode;
        return;
    }

    SPDLOG_INFO_EVERY_N_SEC(
        5,
        "[SHf][D3D12] Scene mode summary mode={} frame={} src=[{}x{} fmt={} flags=0x{:x}] real=[{}x{} fmt={} flags=0x{:x}] normal_dw={}x{} mono_expanded={}",
        shf_scene_mode_name(mode),
        frame_count,
        source_desc.Width,
        source_desc.Height,
        (uint32_t)source_desc.Format,
        (uint32_t)source_desc.Flags,
        real_desc.Width,
        real_desc.Height,
        (uint32_t)real_desc.Format,
        (uint32_t)real_desc.Flags,
        m_backbuffer_size[0],
        m_backbuffer_size[1],
        using_mono_expansion);
}

bool D3D12Component::ensure_shf_mono_scene_texture(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc) {
    if (device == nullptr || m_backbuffer_size[0] == 0 || m_backbuffer_size[1] == 0) {
        return false;
    }

    auto mono_desc = source_desc;
    mono_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    mono_desc.Alignment = 0;
    mono_desc.Width = m_backbuffer_size[0];
    mono_desc.Height = m_backbuffer_size[1];
    mono_desc.DepthOrArraySize = 1;
    mono_desc.MipLevels = 1;
    mono_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    mono_desc.SampleDesc.Count = 1;
    mono_desc.SampleDesc.Quality = 0;
    mono_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    mono_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    mono_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    const auto needs_create =
        m_shf_mono_scene_tex.texture.Get() == nullptr ||
        m_shf_mono_scene_width != mono_desc.Width ||
        m_shf_mono_scene_height != mono_desc.Height ||
        m_shf_mono_scene_format != mono_desc.Format;

    if (!needs_create) {
        return m_shf_mono_scene_tex.srv_heap != nullptr && m_shf_mono_scene_tex.rtv_heap != nullptr;
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    m_shf_mono_scene_tex.reset();

    ComPtr<ID3D12Resource> mono_tex{};
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &mono_desc,
            ENGINE_SRC_COLOR,
            nullptr,
            IID_PPV_ARGS(&mono_tex)))) {
        SPDLOG_ERROR_EVERY_N_SEC(
            1,
            "[SHf][D3D12] Failed to create mono cutscene expansion texture [{}x{} fmt={} flags=0x{:x}]",
            mono_desc.Width,
            mono_desc.Height,
            (uint32_t)mono_desc.Format,
            (uint32_t)mono_desc.Flags);
        return false;
    }

    mono_tex->SetName(L"SHf Mono Cutscene Expansion");

    if (!m_shf_mono_scene_tex.setup(device, mono_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"SHf Mono Cutscene Expansion")) {
        spdlog::error("[SHf][D3D12] Failed to setup mono cutscene expansion texture.");
        m_shf_mono_scene_tex.reset();
        m_shf_mono_scene_width = 0;
        m_shf_mono_scene_height = 0;
        m_shf_mono_scene_format = DXGI_FORMAT_UNKNOWN;
        return false;
    }

    m_shf_mono_scene_width = mono_desc.Width;
    m_shf_mono_scene_height = mono_desc.Height;
    m_shf_mono_scene_format = mono_desc.Format;

    if (!m_shf_mono_scene_commands.ready()) {
        m_shf_mono_scene_commands.setup(L"SHf Mono Cutscene Expansion Commands");
    }

    SPDLOG_WARN(
        "[SHf][D3D12] Created mono cutscene expansion texture [{}x{}] from source [{}x{}]",
        mono_desc.Width,
        mono_desc.Height,
        source_desc.Width,
        source_desc.Height);

    return true;
}

d3d12::TextureContext* D3D12Component::render_shf_mono_scene_texture(ID3D12Device* device) {
    if (!SHF_AUTO_MONO_CINEMATIC ||
        m_game_batch == nullptr ||
        m_game_tex.texture.Get() == nullptr ||
        m_game_tex.srv_heap == nullptr ||
        m_game_tex.srv_heap->Heap() == nullptr) {
        return nullptr;
    }

    const auto source_desc = m_game_tex.texture->GetDesc();

    if (!ensure_shf_mono_scene_texture(device, source_desc) ||
        m_shf_mono_scene_tex.texture.Get() == nullptr ||
        m_shf_mono_scene_tex.rtv_heap == nullptr) {
        return nullptr;
    }

    auto& command_ctx = m_shf_mono_scene_commands;

    if (!command_ctx.ready()) {
        command_ctx.setup(L"SHf Mono Cutscene Expansion Commands");
    }

    if (!command_ctx.ready()) {
        return nullptr;
    }

    command_ctx.wait(INFINITE);

    const float clear_color[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_ctx.clear_rtv(m_shf_mono_scene_tex, clear_color, ENGINE_SRC_COLOR);

    const auto half_width = (LONG)(m_backbuffer_size[0] / 2);
    const auto full_width = (LONG)m_backbuffer_size[0];
    const auto full_height = (LONG)m_backbuffer_size[1];
    const auto source_half_width = (LONG)(source_desc.Width / 2);
    const auto source_height = (LONG)source_desc.Height;

    const RECT left_src{0, 0, source_half_width, source_height};
    const RECT right_src{source_half_width, 0, (LONG)source_desc.Width, source_height};

    auto fit_eye_rect = [&](LONG eye_left, LONG eye_right) {
        RECT dest{eye_left, 0, eye_right, full_height};
        const auto eye_width = (float)(eye_right - eye_left);
        const auto eye_height = (float)full_height;
        const auto source_aspect = source_half_width > 0 && source_height > 0 ? (float)source_half_width / (float)source_height : 1.0f;
        const auto eye_aspect = eye_height > 0.0f ? eye_width / eye_height : source_aspect;

        if (source_aspect > eye_aspect) {
            const auto fitted_height = (LONG)(eye_width / source_aspect);
            const auto y = (full_height - fitted_height) / 2;
            dest.top = y;
            dest.bottom = y + fitted_height;
        } else {
            const auto fitted_width = (LONG)(eye_height * source_aspect);
            const auto x = eye_left + ((LONG)eye_width - fitted_width) / 2;
            dest.left = x;
            dest.right = x + fitted_width;
        }

        return dest;
    };

    const auto left_dest = fit_eye_rect(0, half_width);
    const auto right_dest = fit_eye_rect(half_width, full_width);

    d3d12::render_srv_to_rtv(
        m_game_batch.get(),
        command_ctx.cmd_list.Get(),
        m_game_tex,
        m_shf_mono_scene_tex,
        left_src,
        left_dest,
        ENGINE_SRC_COLOR,
        ENGINE_SRC_COLOR);

    d3d12::render_srv_to_rtv(
        m_game_batch.get(),
        command_ctx.cmd_list.Get(),
        m_game_tex,
        m_shf_mono_scene_tex,
        right_src,
        right_dest,
        ENGINE_SRC_COLOR,
        ENGINE_SRC_COLOR);

    command_ctx.execute();

    SPDLOG_INFO_EVERY_N_SEC(
        2,
        "[SHf][D3D12] Expanded low-res cutscene source [{}x{}] into stereo-safe double-wide [{}x{}]",
        source_desc.Width,
        source_desc.Height,
        m_backbuffer_size[0],
        m_backbuffer_size[1]);

    return &m_shf_mono_scene_tex;
}

vr::EVRCompositorError D3D12Component::on_frame(VR* vr) {
    const auto on_frame_start = std::chrono::steady_clock::now();
    utility::ScopeGuard frame_timing_guard{[&]() {
        m_perf_on_frame.add(std::chrono::steady_clock::now() - on_frame_start);
        log_frame_timing_stats_if_needed(vr);
    }};

    m_last_on_frame = std::chrono::steady_clock::now();
    bool defer_stalker2_transition_openxr = false;

    auto close_openxr_setup_failure_frame = [&]() {
        if (vr->m_openxr == nullptr || !vr->get_runtime()->is_openxr()) {
            return;
        }

        if (vr->m_openxr->close_synced_frame_without_layers("d3d12_setup_failed")) {
            SPDLOG_WARNING_EVERY_N_SEC(
                1,
                "[D3D12 VR] Closed pending OpenXR frame after D3D12 setup failure so the runtime can keep advancing");
        }
    };

    if (m_force_reset || m_last_afr_state != vr->is_using_afr()) {
        if (!setup()) {
            SPDLOG_ERROR_EVERY_N_SEC(1, "[D3D12 VR] Could not set up, trying again next frame");
            close_openxr_setup_failure_frame();
            m_force_reset = true;
            return vr::VRCompositorError_None;
        }

        m_last_afr_state = vr->is_using_afr();
    }

    auto& hook = g_framework->get_d3d12_hook();

    hook->set_next_present_interval(0); // disable vsync for vr
    
    // get device
    auto device = hook->get_device();

    // get command queue
    auto command_queue = hook->get_command_queue();

    // get swapchain
    auto swapchain = hook->get_swap_chain();

    // get back buffer
    ComPtr<ID3D12Resource> backbuffer{};
    ComPtr<ID3D12Resource> real_backbuffer{};
    auto ue4_texture = VR::get()->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer.");
        return vr::VRCompositorError_None;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer.");
        return vr::VRCompositorError_None;
    }

    const auto is_shf_external_backbuffer =
        is_shf_current_game() &&
        g_framework->is_dx12() &&
        backbuffer.Get() != nullptr &&
        real_backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get();
    const auto is_stalker2_ue51_external_backbuffer =
        is_stalker2_current_game() &&
        is_ue_5_1_dx12_backend() &&
        backbuffer.Get() != nullptr &&
        real_backbuffer.Get() != nullptr &&
        backbuffer.Get() != real_backbuffer.Get();
    const auto use_stable_external_backbuffer_copy =
        is_shf_external_backbuffer || is_stalker2_ue51_external_backbuffer;
    const auto volatile_external_source_state =
        is_shf_external_backbuffer ? ENGINE_SRC_COLOR : D3D12_RESOURCE_STATE_RENDER_TARGET;
    const char* stable_external_copy_label =
        is_stalker2_ue51_external_backbuffer ? "Stalker2 UE5.1" : "SHf";
    const wchar_t* stable_external_copy_name =
        is_stalker2_ue51_external_backbuffer ? L"Stalker2 UE5.1 Stable Scene Copy" : L"SHf Stable Scene Copy";
    const auto skip_in_place_ui_invert = false;
    m_skip_spectator_view_for_volatile_external_rt = is_shf_external_backbuffer;
    auto scene_source_state = use_stable_external_backbuffer_copy ? ENGINE_SRC_COLOR : D3D12_RESOURCE_STATE_RENDER_TARGET;

    if (is_stalker2_ue51_external_backbuffer) {
        static auto s_stalker2_last_d3d12_frame = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();

        if (s_stalker2_last_d3d12_frame.time_since_epoch().count() != 0 &&
            now - s_stalker2_last_d3d12_frame > std::chrono::milliseconds{100})
        {
            vr->note_stalker2_transition_stress("d3d12_frame_gap");
        }

        s_stalker2_last_d3d12_frame = now;
    }

    const auto ui_invert_alpha = vr->get_overlay_component().get_ui_invert_alpha();

    // Update the UI overlay.
    auto runtime = vr->get_runtime();
    const auto openxr_runtime = runtime->is_openxr() ? vr->m_openxr.get() : nullptr;
    const auto debug_submit_empty_frame = openxr_runtime != nullptr && openxr_runtime->debug_submit_empty_frame->value();
    const auto debug_skip_scene_copy = openxr_runtime != nullptr && openxr_runtime->debug_skip_scene_copy->value();
    const auto debug_skip_ui_copy = openxr_runtime != nullptr && openxr_runtime->debug_skip_ui_copy->value();
    const auto debug_disable_depth_submit = openxr_runtime != nullptr && openxr_runtime->debug_disable_depth_submit->value();
    const auto suppress_scene_copy = debug_submit_empty_frame || debug_skip_scene_copy;
    const auto suppress_ui_copy = debug_submit_empty_frame || debug_skip_ui_copy;

    const auto is_same_frame = m_last_rendered_frame > 0 && m_last_rendered_frame == vr->m_render_frame_count;
    m_last_rendered_frame = vr->m_render_frame_count;

    const auto is_actually_afr = vr->is_using_afr();
    const auto is_afr = !is_same_frame && vr->is_using_afr();
    const auto is_left_eye_frame = is_afr && vr->m_render_frame_count % 2 == vr->m_left_eye_interval;
    const auto is_right_eye_frame = !is_afr || vr->m_render_frame_count % 2 == vr->m_right_eye_interval;

    // Sometimes this can happen if pipeline execution does not go exactly as planned
    // so we need to resynchronized or begin the frame again.
    if (runtime->ready()) {
        if (runtime->is_openxr()) {
            // Keep xrWaitFrame ownership where it already is, but do not let the D3D12
            // path begin the frame here. We open it at the first OpenXR copy/acquire.
            defer_stalker2_transition_openxr =
                vr->should_defer_stalker2_openxr_frame_for_transition("d3d12_pre_wait");

            if (!defer_stalker2_transition_openxr) {
                runtime->synchronize_frame(std::nullopt, VRRuntime::SyncFrameCallsite::RuntimeFixFrame);
            }
        } else {
            runtime->fix_frame();
        }
    }

    const auto& ffsr = VR::get()->m_fake_stereo_hook;
    const auto ui_target = ffsr->get_render_target_manager()->get_ui_target();

    const auto frame_count = vr->m_render_frame_count;

    if (m_game_tex.texture.Get() == nullptr && backbuffer.Get() == real_backbuffer.Get()) {
        spdlog::info("[VR] Setting up game texture as copy of backbuffer");
        
        ComPtr<ID3D12Resource> backbuffer_copy{};
        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        auto desc = backbuffer->GetDesc();
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        m_backbuffer_copy.reset();

        ComPtr<ID3D12Resource> backbuffer_copy2{};

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy2)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_backbuffer_copy.setup(device, backbuffer_copy2.Get(), std::nullopt, std::nullopt, L"Backbuffer Copy")) {
            spdlog::error("[VR] Failed to fully setup backbuffer copy.");
            m_backbuffer_copy.reset();
        }

        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // UE backbuffer is not VR compatible, so we need to copy it to a new texture with this one.

        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&backbuffer_copy)))) {
            spdlog::error("[VR] Failed to create backbuffer copy.");
            return vr::VRCompositorError_None;
        }

        if (!m_game_tex.setup(device, backbuffer_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
            spdlog::error("[VR] Failed to fully setup game texture.");
            m_game_tex.reset();
        } else {
            for (auto& commands : m_game_tex_commands) {
                commands.setup(L"Game Texture Commands");
            }
        }
    } else if (backbuffer.Get() != real_backbuffer.Get() && (use_stable_external_backbuffer_copy || m_game_tex.texture.Get() != backbuffer.Get() || !texture_context_has_views(m_game_tex))) {
        log_shf_texture_reference_rebuild(backbuffer.Get(), real_backbuffer.Get(), m_game_tex.texture.Get(), frame_count);

        if (use_stable_external_backbuffer_copy) {
            const auto source_desc = backbuffer->GetDesc();
            const auto needs_copy_texture =
                m_game_tex.texture.Get() == nullptr ||
                !shf_texture_desc_matches(m_game_tex.texture->GetDesc(), source_desc);

            if (needs_copy_texture) {
                SPDLOG_WARN("[{}][D3D12] Creating owned stable scene copy for volatile external RT [{}x{} fmt={} flags=0x{:x}]",
                    stable_external_copy_label, source_desc.Width, source_desc.Height, (uint32_t)source_desc.Format, (uint32_t)source_desc.Flags);

                D3D12_HEAP_PROPERTIES heap_props{};
                heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
                heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
                heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

                auto copy_desc = source_desc;
                copy_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
                copy_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

                ComPtr<ID3D12Resource> stable_copy{};

                if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &copy_desc, ENGINE_SRC_COLOR, nullptr, IID_PPV_ARGS(&stable_copy)))) {
                    SPDLOG_ERROR_EVERY_N_SEC(1,
                        "[{}][D3D12] Failed to create owned stable scene copy [{}x{} fmt={} flags=0x{:x}]; falling back to volatile RT path",
                        stable_external_copy_label, copy_desc.Width, copy_desc.Height, (uint32_t)copy_desc.Format, (uint32_t)copy_desc.Flags);
                    m_game_tex.reset();
                } else if (!m_game_tex.setup(device, stable_copy.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, stable_external_copy_name)) {
                    spdlog::error("[{}][D3D12] Failed to setup owned stable scene copy.", stable_external_copy_label);
                    m_game_tex.reset();
                } else {
                    for (auto& commands : m_game_tex_commands) {
                        if (!commands.ready()) {
                            commands.setup(L"SHf Stable Scene Copy Commands");
                        }
                    }
                }
            }

            if (m_game_tex.texture.Get() != nullptr) {
                const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
                auto& command_ctx = m_game_tex_commands[idx];

                if (!command_ctx.ready()) {
                    command_ctx.setup(L"SHf Stable Scene Copy Commands");
                }

                if (command_ctx.ready()) {
                    command_ctx.wait(INFINITE);
                    command_ctx.copy(backbuffer.Get(), m_game_tex.texture.Get(), volatile_external_source_state, ENGINE_SRC_COLOR);
                    command_ctx.execute();

                    SPDLOG_INFO_EVERY_N_SEC(2,
                        "[{}][D3D12] Copied volatile external RT into owned stable scene texture for HMD/mirror/2D",
                        stable_external_copy_label);

                    m_skip_spectator_view_for_volatile_external_rt = false;
                    backbuffer = m_game_tex.texture;
                    scene_source_state = ENGINE_SRC_COLOR;
                }
            }

            if (m_game_tex.texture.Get() == nullptr) {
                SPDLOG_WARNING_EVERY_N_SEC(
                    1,
                    "[{}][D3D12] Stable scene copy unavailable; falling back to volatile external RT reference",
                    stable_external_copy_label);
                scene_source_state = volatile_external_source_state;

                if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
                    spdlog::error("[VR] Failed to fully setup fallback game texture reference.");
                    m_game_tex.reset();
                }
            }
        } else {
            spdlog::info("[VR] Setting up game texture as reference to original");

            if (!m_game_tex.setup(device, backbuffer.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Game Texture")) {
                spdlog::error("[VR] Failed to fully setup game texture.");
                m_game_tex.reset();
            }
        }
    }

    if (vr->is_native_stereo_fix_enabled()) {
        const auto scene_capture = ffsr->get_render_target_manager()->get_scene_capture_render_target();
        const auto scene_capture_rt = scene_capture != nullptr ? (ID3D12Resource*)scene_capture->get_native_resource() : nullptr;

        if (is_avowed_current_game()) {
            SPDLOG_INFO_EVERY_N_SEC(
                2,
                "[Avowed][D3D12][NativeStereoFix] Scene capture texture state: rhi={} native={} cached={} game_tex={}",
                (uintptr_t)scene_capture,
                (uintptr_t)scene_capture_rt,
                (uintptr_t)m_scene_capture_tex.texture.Get(),
                (uintptr_t)m_game_tex.texture.Get());
        }

        if (scene_capture_rt != nullptr && m_scene_capture_tex.texture.Get() != scene_capture_rt) {
            spdlog::info("[VR] Setting up scene capture texture as reference to original");

            if (!m_scene_capture_tex.setup(device, scene_capture_rt, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"Scene Capture Texture")) {
                spdlog::error("[VR] Failed to fully setup scene capture texture.");
                m_scene_capture_tex.reset();
            }
        }

        if (scene_capture_rt == nullptr && m_scene_capture_tex.texture.Get() != nullptr) {
            spdlog::info("[VR] Resetting scene capture texture");

            m_scene_capture_tex.reset();
        }
    } else {
        m_scene_capture_tex.reset();
    }

    // We need to render the scene capture texture to the right side of the double wide texture
    auto pre_render = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        if (render_target == nullptr) {
            return;
        }

        // Also the same for right, even though it's not a double wide texture
        D3D12_BOX left_src_box{
            .left = 0,
            .top = 0,
            .front = 0,
            .right = m_backbuffer_size[0] / 2,
            .bottom = m_backbuffer_size[1],
            .back = 1
        };

        commands.copy_region_stereo(
            m_game_tex.texture.Get(), m_scene_capture_tex.texture.Get(), render_target,
            &left_src_box, &left_src_box,
            0, 0, 0, m_backbuffer_size[0] / 2, 0, 0,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_RENDER_TARGET
        );
    };

    // For copying the real backbuffer if we need to
    if (m_game_tex.texture.Get() != nullptr && backbuffer == real_backbuffer) {
        const auto idx = swapchain->GetCurrentBackBufferIndex() % m_game_tex_commands.size();
        auto& command_ctx = m_game_tex_commands[idx];
        if (command_ctx.cmd_list != nullptr) {
            command_ctx.wait(INFINITE);
            float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
            command_ctx.clear_rtv(m_game_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_ctx.copy(real_backbuffer.Get(), m_backbuffer_copy.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            //m_game_tex_commands[idx].copy(backbuffer.Get(), m_game_tex.texture.Get(), D3D12_RESOURCE_STATE_PRESENT, ENGINE_SRC_COLOR);
            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                command_ctx.cmd_list.Get(),
                m_backbuffer_copy,
                m_game_tex,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_RENDER_TARGET
            );
            command_ctx.execute();
        }

        backbuffer = m_game_tex.texture;
    }

    auto* effective_game_tex = &m_game_tex;
    bool shf_using_mono_expansion = false;
    auto shf_scene_mode = ShfSceneMode::Unknown;

    if (is_shf_external_backbuffer && m_game_tex.texture.Get() != nullptr && real_backbuffer.Get() != nullptr) {
        const auto source_desc = m_game_tex.texture->GetDesc();
        const auto real_desc = real_backbuffer->GetDesc();
        shf_scene_mode = classify_shf_scene_mode(source_desc, real_desc);

        if (SHF_AUTO_MONO_CINEMATIC && shf_scene_mode == ShfSceneMode::Mono2D) {
            if (auto* mono_scene = render_shf_mono_scene_texture(device); mono_scene != nullptr && mono_scene->texture.Get() != nullptr) {
                effective_game_tex = mono_scene;
                backbuffer = mono_scene->texture;
                scene_source_state = ENGINE_SRC_COLOR;
                shf_using_mono_expansion = true;
            } else {
                SPDLOG_ERROR_EVERY_N_SEC(
                    1,
                    "[SHf][D3D12] Mono scene source detected but expansion texture was unavailable; leaving existing stereo copy path active");
            }
        }

        log_shf_scene_mode_if_needed(shf_scene_mode, source_desc, real_desc, frame_count, shf_using_mono_expansion);
    }

    if (ui_target != nullptr) {
        if (m_game_ui_tex.texture.Get() != ui_target->get_native_resource()) {
            if (!m_game_ui_tex.setup(device, 
                (ID3D12Resource*)ui_target->get_native_resource(), 
                DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
                L"Game UI Texture"))
            {
                spdlog::error("[VR] Failed to fully setup game UI texture.");
                m_game_ui_tex.reset();
            }
        }

        // Recreate UI texture if needed
        if (!vr->is_extreme_compatibility_mode_enabled()) {
            const auto native = (ID3D12Resource*)ui_target->get_native_resource();
            const auto is_same_native = native == m_last_checked_native;
            m_last_checked_native = native;

            if (native != nullptr && !is_same_native) {
                const auto desc = native->GetDesc();

                if (runtime->is_openxr()) {
                    if (auto it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);
                        it != vr->m_openxr->swapchains.end()) 
                    {
                        const auto& uisc = it->second;
                        if (desc.Width != uisc.width ||
                            desc.Height != uisc.height)
                        {
                            SPDLOG_INFO_EVERY_N_SEC(1, "[OpenXR] UI size changed, recreating [{}x{}]->[{}x{}]", desc.Width, desc.Height, uisc.width, uisc.height);
                            ffsr->set_should_recreate_textures(true);
                        }
                    }
                } else if (m_game_ui_tex.texture != nullptr) {
                    const auto ui_desc = m_game_ui_tex.texture->GetDesc();

                    if (desc.Width != ui_desc.Width || desc.Height != ui_desc.Height) {
                        SPDLOG_INFO_EVERY_N_SEC(1, "[OpenVR] UI size changed, recreating texture [{}x{}]->[{}x{}]", desc.Width, desc.Height, ui_desc.Width, ui_desc.Height);
                        ffsr->set_should_recreate_textures(true);
                    }
                }
            } else if (native == nullptr) {
                spdlog::error("[VR] Recreating UI texture because native resource is null");
                ffsr->set_should_recreate_textures(true);
            }
        }
    } else {
        const bool keep_pending_ue57_ui =
            ffsr->get_render_target_manager()->get_dedicated_ui_width() != 0 &&
            ffsr->get_render_target_manager()->get_dedicated_ui_height() != 0 &&
            ffsr->get_render_target_manager()->is_dedicated_ui_target_pending();

        if (!keep_pending_ue57_ui) {
            m_game_ui_tex.reset(); // Probably fixes non-resident errors.
        }
    }

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const auto is_2d_screen = vr->is_using_2d_screen();
    const auto shf_auto_2d_screen =
        SHF_AUTO_2D_SCREEN_FROM_MONO_CINEMATIC &&
        is_shf_external_backbuffer &&
        shf_scene_mode == ShfSceneMode::Mono2D &&
        m_game_tex.texture.Get() != nullptr &&
        m_game_tex.srv_heap != nullptr;
    const auto use_2d_screen = is_2d_screen || shf_auto_2d_screen;

    if (shf_auto_2d_screen) {
        SPDLOG_INFO_EVERY_N_SEC(
            2,
            "[SHf][D3D12] Auto 2D screen active for detected Mono2D cinematic segment");
    }

    if (use_2d_screen && effective_game_tex != nullptr && effective_game_tex->texture.Get() != nullptr) {
        ensure_2d_screen_textures(device, effective_game_tex->texture->GetDesc());
    }

    auto draw_2d_view = [&](d3d12::CommandContext& commands, ID3D12Resource* render_target) {
        auto& view_game_tex = effective_game_tex != nullptr ? *effective_game_tex : m_game_tex;
        const auto view_game_tex_clear_state =
            (is_shf_external_backbuffer || shf_using_mono_expansion) ? ENGINE_SRC_COLOR : D3D12_RESOURCE_STATE_RENDER_TARGET;

        if (ui_invert_alpha > 0.0f && !skip_in_place_ui_invert && m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
            const std::array<float, 4> blend_factor{ 1.0f, 1.0f, 1.0f, ui_invert_alpha };
            const DirectX::XMFLOAT4 invert_alpha_tint{ 1.0f, 1.0f, 1.0f, ui_invert_alpha };
            d3d12::render_srv_to_rtv(
                m_ui_batch_alpha_invert.get(),
                commands.cmd_list.Get(),
                m_game_ui_tex,
                m_game_ui_tex,
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR,
                blend_factor,
                invert_alpha_tint);
        }

        draw_spectator_view(commands.cmd_list.Get(), is_right_eye_frame, &view_game_tex);

        const auto has_2d_screen_textures =
            m_2d_screen_tex[0].texture.Get() != nullptr &&
            m_2d_screen_tex[1].texture.Get() != nullptr &&
            m_2d_screen_tex[0].rtv_heap != nullptr &&
            m_2d_screen_tex[1].rtv_heap != nullptr;

        if (use_2d_screen && has_2d_screen_textures && view_game_tex.texture.Get() != nullptr && view_game_tex.srv_heap != nullptr) {
            // Clear previous frame
            for (auto& screen : m_2d_screen_tex) {
                commands.clear_rtv(screen, clear_color, ENGINE_SRC_COLOR);
            }

            const auto use_shf_flat_screen_source = is_shf_current_game();
            auto* screen_source_tex = &view_game_tex;

            if (use_shf_flat_screen_source &&
                shf_scene_mode == ShfSceneMode::Mono2D &&
                m_game_tex.texture.Get() != nullptr &&
                m_game_tex.srv_heap != nullptr) {
                screen_source_tex = &m_game_tex;
            }

            const auto view_desc = screen_source_tex->texture->GetDesc();
            RECT left_source_rect{0, 0, (LONG)((float)m_backbuffer_size[0] / 2.0f), (LONG)m_backbuffer_size[1]};
            RECT right_source_rect{(LONG)((float)m_backbuffer_size[0] / 2.0f), 0, (LONG)((float)m_backbuffer_size[0]), (LONG)m_backbuffer_size[1]};
            std::optional<RECT> screen_dest_rect = std::nullopt;

            if (use_shf_flat_screen_source) {
                const auto source_width = (LONG)view_desc.Width;
                const auto source_height = (LONG)view_desc.Height;
                left_source_rect = RECT{0, 0, source_width, source_height};

                // Mono2D uses the original wide cinematic source. Other manual 2D cases use a matched single eye.
                if (shf_scene_mode != ShfSceneMode::Mono2D &&
                    view_desc.Width >= (uint64_t)view_desc.Height * 2 &&
                    view_desc.Width >= 2) {
                    left_source_rect.right = (LONG)(view_desc.Width / 2);
                }

                right_source_rect = left_source_rect;
                const auto screen_desc = m_2d_screen_tex[0].texture->GetDesc();
                const auto source_rect_width = (float)(left_source_rect.right - left_source_rect.left);
                const auto source_rect_height = (float)(left_source_rect.bottom - left_source_rect.top);
                const auto screen_width = (float)screen_desc.Width;
                const auto screen_height = (float)screen_desc.Height;
                RECT dest_rect{0, 0, (LONG)screen_desc.Width, (LONG)screen_desc.Height};

                if (source_rect_width > 0.0f && source_rect_height > 0.0f && screen_width > 0.0f && screen_height > 0.0f) {
                    const auto source_aspect = source_rect_width / source_rect_height;
                    const auto screen_aspect = screen_width / screen_height;

                    if (source_aspect > screen_aspect) {
                        const auto fitted_height = (LONG)(screen_width / source_aspect);
                        const auto y = ((LONG)screen_desc.Height - fitted_height) / 2;
                        dest_rect.top = y;
                        dest_rect.bottom = y + fitted_height;
                    } else {
                        const auto fitted_width = (LONG)(screen_height * source_aspect);
                        const auto x = ((LONG)screen_desc.Width - fitted_width) / 2;
                        dest_rect.left = x;
                        dest_rect.right = x + fitted_width;
                    }

                    screen_dest_rect = dest_rect;
                }

                SPDLOG_INFO_EVERY_N_SEC(
                    2,
                    "[SHf][D3D12] 2D screen using matched mono source mode={} auto={} tex=[{}x{} fmt={}] src=[{},{} -> {},{}] dst=[{},{} -> {},{}]",
                    shf_scene_mode_name(m_shf_scene_mode),
                    shf_auto_2d_screen,
                    view_desc.Width,
                    view_desc.Height,
                    (uint32_t)view_desc.Format,
                    left_source_rect.left,
                    left_source_rect.top,
                    left_source_rect.right,
                    left_source_rect.bottom,
                    screen_dest_rect ? screen_dest_rect->left : 0,
                    screen_dest_rect ? screen_dest_rect->top : 0,
                    screen_dest_rect ? screen_dest_rect->right : (LONG)m_2d_screen_tex[0].texture->GetDesc().Width,
                    screen_dest_rect ? screen_dest_rect->bottom : (LONG)m_2d_screen_tex[0].texture->GetDesc().Height);
            }

            d3d12::render_srv_to_rtv(
                m_game_batch.get(),
                commands.cmd_list.Get(),
                *screen_source_tex,
                m_2d_screen_tex[0],
                left_source_rect,
                screen_dest_rect,
                ENGINE_SRC_COLOR,
                ENGINE_SRC_COLOR
            );

            if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                d3d12::render_srv_to_rtv(
                    m_game_batch.get(),
                    commands.cmd_list.Get(),
                    m_game_ui_tex,
                    m_2d_screen_tex[0],
                    ENGINE_SRC_COLOR,
                    ENGINE_SRC_COLOR
                );
            }

            if (!is_afr) {
                if (!use_shf_flat_screen_source && m_scene_capture_tex.texture.Get() != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_scene_capture_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                } else {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        *screen_source_tex,
                        m_2d_screen_tex[1],
                        right_source_rect,
                        screen_dest_rect,
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }

                if (m_game_ui_tex.texture.Get() != nullptr && m_game_ui_tex.srv_heap != nullptr) {
                    d3d12::render_srv_to_rtv(
                        m_game_batch.get(),
                        commands.cmd_list.Get(),
                        m_game_ui_tex,
                        m_2d_screen_tex[1],
                        ENGINE_SRC_COLOR,
                        ENGINE_SRC_COLOR
                    );
                }
            }

            // Clear the RT so the entire background is black when submitting to the compositor
            commands.clear_rtv(view_game_tex, (float*)&clear_color, view_game_tex_clear_state);

            if (m_scene_capture_tex.texture.Get() != nullptr) {
                commands.clear_rtv(m_scene_capture_tex, (float*)&clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
        }
    };

    // Draws the spectator view
    auto clear_rt = [&](d3d12::CommandContext& commands) {
		if (m_game_ui_tex.texture.Get() == nullptr) {
            return;
        }
		
        const float ui_clear_color[] = { 0.0f, 0.0f, 0.0f, ui_invert_alpha };
        commands.clear_rtv(m_game_ui_tex, (float*)&ui_clear_color, ENGINE_SRC_COLOR);
    };

    auto ensure_openxr_frame_began = [&](const char* caller) -> bool {
        if (!runtime->is_openxr() || !vr->m_openxr->can_run_frame_loop()) {
            return false;
        }

        if (vr->m_openxr->frame_began) {
            return true;
        }

        if (defer_stalker2_transition_openxr && !vr->m_openxr->frame_synced) {
            return false;
        }

        const auto begin_result = vr->m_openxr->begin_frame(caller);

        if (!vr->m_openxr->frame_began) {
            SPDLOG_INFO_EVERY_N_SEC(
                1,
                "[OpenXR] Skipping D3D12 OpenXR copy because begin_frame did not leave a frame open: {}",
                vr->m_openxr->get_result_string(begin_result)
            );
            return false;
        }

        return true;
    };

    if (runtime->is_openvr() && m_openvr.ui_tex.texture.Get() != nullptr) {
        const auto ui_copy_start = std::chrono::steady_clock::now();
        utility::ScopeGuard ui_copy_timing_guard{[&]() {
            m_perf_ui_copy.add(std::chrono::steady_clock::now() - ui_copy_start);
        }};

        m_openvr.ui_tex.commands.wait(INFINITE);

        draw_2d_view(m_openvr.ui_tex.commands, nullptr);

        if (is_right_eye_frame) {
            if (use_2d_screen) {
                m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            } else if (ui_target != nullptr) {
                m_openvr.ui_tex.commands.copy((ID3D12Resource*)ui_target->get_native_resource(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
            }
        } else if (use_2d_screen) {
            m_openvr.ui_tex.commands.copy(m_2d_screen_tex[0].texture.Get(), m_openvr.ui_tex.texture.Get(), ENGINE_SRC_COLOR);
        }

        clear_rt(m_openvr.ui_tex.commands);
        m_openvr.ui_tex.commands.execute();
    } else if (runtime->is_openxr() && vr->m_openxr->can_run_frame_loop() && ensure_openxr_frame_began("d3d12_first_copy")) {
        const auto ui_copy_start = std::chrono::steady_clock::now();
        utility::ScopeGuard ui_copy_timing_guard{[&]() {
            m_perf_ui_copy.add(std::chrono::steady_clock::now() - ui_copy_start);
        }};

        if (suppress_ui_copy) {
            SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Skipping UI copy for perf isolation");
        } else {
            if (is_right_eye_frame) {
                if (use_2d_screen) {
                    if (is_afr) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                    } else {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, std::nullopt, ENGINE_SRC_COLOR);
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, m_2d_screen_tex[1].texture.Get(), std::nullopt, clear_rt, ENGINE_SRC_COLOR);
                    }
                } else if (ui_target != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, (ID3D12Resource*)ui_target->get_native_resource(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
                }

                auto fw_rt = g_framework->get_rendertarget_d3d12();

                if (fw_rt && g_framework->is_drawing_anything()) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, g_framework->get_rendertarget_d3d12().Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
            } else if (use_2d_screen) {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, m_2d_screen_tex[0].texture.Get(), draw_2d_view, clear_rt, ENGINE_SRC_COLOR);
            } else if (m_game_ui_tex.commands.ready()) {
                m_game_ui_tex.commands.wait(INFINITE);
                draw_2d_view(m_game_ui_tex.commands, nullptr);
                clear_rt(m_game_ui_tex.commands);
                m_game_ui_tex.commands.execute();
            }
        }
    }

    /*else if (m_game_tex.texture.Get() != nullptr) {
        m_game_tex.commands.wait(INFINITE);
        draw_spectator_view(m_game_tex.commands.cmd_list.Get(), is_right_eye_frame);
        m_game_tex.commands.execute();
    }*/

    ComPtr<ID3D12Resource> scene_depth_tex{};

    if (vr->is_depth_enabled() && runtime->is_depth_allowed()) {
        auto& rt_pool = vr->get_render_target_pool_hook();
        scene_depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();

            if (runtime->is_openxr()) {
                if (vr->m_openxr->needs_depth_resize(desc.Width, desc.Height) || m_openxr.made_depth_with_null_defaults) {
                    uint32_t reasons = SWAPCHAIN_RECREATE_DEPTH_EXTENT;
                    if (m_openxr.made_depth_with_null_defaults) {
                        reasons |= SWAPCHAIN_RECREATE_DEPTH_NULL_DEFAULTS;
                    }
                    log_openxr_swapchain_recreate(vr, reasons, (uint32_t)desc.Width, (uint32_t)desc.Height);
                    prepare_openxr_swapchain_recreate(vr, reasons);
                    m_openxr.create_swapchains(); // recreate swapchains to match the new depth size
                }
            }
        }

    #ifdef AFR_DEPTH_TEMP_DISABLED
        if (is_actually_afr) {
            scene_depth_tex.Reset();
        }
    #endif
    }

    if (shf_using_mono_expansion && scene_depth_tex != nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf][D3D12] Suppressing depth submit while mono cutscene expansion is active");
        scene_depth_tex.Reset();
    }

    if ((debug_disable_depth_submit || debug_submit_empty_frame || debug_skip_scene_copy) && scene_depth_tex != nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Suppressing depth submit for perf isolation");
        scene_depth_tex.Reset();
    }

    // If m_frame_count is even, we're rendering the left eye.
    if (is_left_eye_frame) {
        m_submitted_left_eye = true;

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->can_run_frame_loop()) {
            const auto swapchain_copy_start = std::chrono::steady_clock::now();
            utility::ScopeGuard swapchain_copy_timing_guard{[&]() {
                m_perf_swapchain_copy.add(std::chrono::steady_clock::now() - swapchain_copy_start);
            }};

            D3D12_BOX src_box{};
            src_box.left = 0;
            src_box.top = 0;
            src_box.bottom = m_backbuffer_size[1];
            src_box.front = 0;
            src_box.back = 1;

            if (vr->is_extreme_compatibility_mode_enabled()) {
                src_box.right = m_backbuffer_size[0];
            } else {
                src_box.right = m_backbuffer_size[0] / 2;
            }

            if (suppress_scene_copy) {
                SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Skipping left-eye scene copy for perf isolation");
                if (!debug_submit_empty_frame) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, nullptr, scene_source_state, nullptr);
                }
            } else {
                m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), scene_source_state, &src_box);

                if (scene_depth_tex != nullptr) {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left eye texture
        if (runtime->is_openvr()) {
            m_openvr.copy_left(backbuffer.Get(), scene_source_state);

            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            vr::D3D12TextureData_t left {
                m_openvr.get_left().texture.Get(),
                command_queue,
                0
            };
            
            vr::VRTextureWithPose_t left_eye{
                (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                           runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                return e;
            }
        }
    } else {
        utility::ScopeGuard __{[&]() {
            m_submitted_left_eye = false;
        }};

        // OpenXR texture
        if (runtime->is_openxr() && vr->m_openxr->can_run_frame_loop()) {
            const auto swapchain_copy_start = std::chrono::steady_clock::now();
            utility::ScopeGuard swapchain_copy_timing_guard{[&]() {
                m_perf_swapchain_copy.add(std::chrono::steady_clock::now() - swapchain_copy_start);
            }};

            if (is_actually_afr && !is_afr && !m_submitted_left_eye) {
                D3D12_BOX src_box{};
                src_box.left = 0;
                src_box.top = 0;
                src_box.bottom = m_backbuffer_size[1];
                src_box.front = 0;
                src_box.back = 1;

                if (vr->is_extreme_compatibility_mode_enabled()) {
                    src_box.right = m_backbuffer_size[0];
                } else {
                    src_box.right = m_backbuffer_size[0] / 2;
                }

                if (suppress_scene_copy) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Skipping staged left-eye scene copy for perf isolation");
                    if (!debug_submit_empty_frame) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, nullptr, scene_source_state, nullptr);
                    }
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, backbuffer.Get(), scene_source_state, &src_box);

                    if (scene_depth_tex != nullptr) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                    }
                }
            }

            if (is_actually_afr) {
                D3D12_BOX src_box{};

                if (!vr->is_extreme_compatibility_mode_enabled()) {
                    if (!is_afr) {
                        src_box.left = m_backbuffer_size[0] / 2;
                        src_box.right = m_backbuffer_size[0];
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    } else { // Copy the left eye on AFR
                        src_box.left = 0;
                        src_box.right = m_backbuffer_size[0] / 2;
                        src_box.top = 0;
                        src_box.bottom = m_backbuffer_size[1];
                        src_box.front = 0;
                        src_box.back = 1;
                    }   
                } else {
                    src_box.left = 0;
                    src_box.right = m_backbuffer_size[0];
                    src_box.top = 0;
                    src_box.bottom = m_backbuffer_size[1];
                    src_box.front = 0;
                    src_box.back = 1;
                }

                if (suppress_scene_copy) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Skipping right-eye scene copy for perf isolation");
                    if (!debug_submit_empty_frame) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, nullptr, scene_source_state, nullptr);
                    }
                } else {
                    m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, backbuffer.Get(), scene_source_state, &src_box);

                    if (scene_depth_tex != nullptr) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                    }
                }
            } else {
                // Copy over the entire double wide instead
                if (suppress_scene_copy) {
                    SPDLOG_INFO_EVERY_N_SEC(2, "[OpenXR][debug] Skipping double-wide scene copy for perf isolation");
                    if (!debug_submit_empty_frame) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, scene_source_state, nullptr);
                    }
                } else {
                    if (m_scene_capture_tex.texture.Get() == nullptr || shf_using_mono_expansion) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, backbuffer.Get(), scene_source_state, nullptr);
                    } else {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, nullptr, pre_render, std::nullopt, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
                    }

                    if (scene_depth_tex != nullptr) {
                        m_openxr.copy((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, scene_depth_tex.Get(), ENGINE_SRC_DEPTH, nullptr);
                    }
                }
            }
        }

        // OpenVR texture
        // Copy the back buffer to the left and right eye textures.
        if (runtime->is_openvr()) {
            auto openvr = vr->get_runtime<runtimes::OpenVR>();
            const auto submit_pose = openvr->get_pose_for_submit();

            if (!is_afr) {
                m_openvr.copy_left(backbuffer.Get(), scene_source_state);

                vr::D3D12TextureData_t left {
                    m_openvr.get_left().texture.Get(),
                    command_queue,
                    0
                };

                vr::VRTextureWithPose_t left_eye{
                    (void*)&left, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                    submit_pose
                };
                const auto left_bounds = vr::VRTextureBounds_t{runtime->view_bounds[0][0], runtime->view_bounds[0][2],
                                                               runtime->view_bounds[0][1], runtime->view_bounds[0][3]};
                auto e = vr::VRCompositor()->Submit(vr::Eye_Left, &left_eye, &left_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);

                if (e != vr::VRCompositorError_None) {
                    spdlog::error("[VR] VRCompositor failed to submit left eye: {}", (int)e);
                    //return e; // dont return because it will just completely stop us from even getting to the right eye which could be catastrophic
                }
            }

            if (!is_afr) {
                if (m_scene_capture_tex.texture.Get() == nullptr) {
                    m_openvr.copy_right(backbuffer.Get(), scene_source_state);
                } else {
                    m_openvr.copy_left_to_right(m_scene_capture_tex.texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            } else {
                m_openvr.copy_left_to_right(backbuffer.Get(), scene_source_state);
            }

            vr::D3D12TextureData_t right {
                m_openvr.get_right().texture.Get(),
                command_queue,
                0
            };

            vr::VRTextureWithPose_t right_eye{
                (void*)&right, vr::TextureType_DirectX12, vr::ColorSpace_Auto,
                submit_pose
            };
            const auto right_bounds = vr::VRTextureBounds_t{runtime->view_bounds[1][0], runtime->view_bounds[1][2],
                                                            runtime->view_bounds[1][1], runtime->view_bounds[1][3]};
            auto e = vr::VRCompositor()->Submit(vr::Eye_Right, &right_eye, &right_bounds, vr::EVRSubmitFlags::Submit_TextureWithPose);
            runtime->frame_synced = false;

            if (e != vr::VRCompositorError_None) {
                spdlog::error("[VR] VRCompositor failed to submit right eye: {}", (int)e);
                return e;
            } else {
                vr->m_submitted = true;
            }

            ++m_openvr.texture_counter;
        }
    }

    if (is_right_eye_frame) {
        if ((runtime->ready() && vr->get_synchronize_stage() == VR::SynchronizeStage::VERY_LATE) || !runtime->got_first_sync) {
            //vr->update_hmd_state();
        }
    }

    vr::EVRCompositorError e = vr::EVRCompositorError::VRCompositorError_None;

    if (is_right_eye_frame) {
        ////////////////////////////////////////////////////////////////////////////////
        // OpenXR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openxr() && vr->m_openxr->can_run_frame_loop()) {
            const auto openxr_submit_start = std::chrono::steady_clock::now();
            utility::ScopeGuard openxr_submit_timing_guard{[&]() {
                m_perf_openxr_submit.add(std::chrono::steady_clock::now() - openxr_submit_start);
            }};

            if (defer_stalker2_transition_openxr && !vr->m_openxr->frame_synced && !vr->m_openxr->frame_began) {
                SPDLOG_INFO_EVERY_N_SEC(
                    1,
                    "[Stalker2][OpenXR] Skipping D3D12 OpenXR submit for transition guard because no frame was synchronized");
                return e;
            }

            if (!vr->m_openxr->frame_began) {
                const auto begin_result = vr->m_openxr->begin_frame("d3d12_submit");

                if (!vr->m_openxr->frame_began) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        1,
                        "[OpenXR] Skipping D3D12 submit because begin_frame did not leave a frame open: {}",
                        vr->m_openxr->get_result_string(begin_result)
                    );
                    return e;
                }
            }

            vr->m_openxr->refresh_stale_pose_before_submit(frame_count, "d3d12_submit");

            std::vector<XrCompositionLayerBaseHeader*> quad_layers{};

            auto& openxr_overlay = vr->get_overlay_component().get_openxr();

            if (!suppress_ui_copy && use_2d_screen) {
                if (shf_auto_2d_screen) {
                    SPDLOG_INFO_EVERY_N_SEC(
                        2,
                        "[SHf][D3D12] Submitting auto 2D screen as eye-specific OpenXR slate layers");
                }

                const auto left_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI, XrEyeVisibility::XR_EYE_VISIBILITY_LEFT);
                const auto right_layer = openxr_overlay.generate_slate_layer(runtimes::OpenXR::SwapchainIndex::UI_RIGHT, XrEyeVisibility::XR_EYE_VISIBILITY_RIGHT);

                if (left_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&left_layer->get());
                }

                if (right_layer && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT)) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&right_layer->get());
                }
            } else if (!suppress_ui_copy && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::UI)) {
                const auto slate_layer = openxr_overlay.generate_slate_layer();

                if (slate_layer) {
                    quad_layers.push_back(&slate_layer->get());
                }   
            }
            
            if (!suppress_ui_copy && m_openxr.ever_acquired((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI)) {
                const auto framework_quad = openxr_overlay.generate_framework_ui_quad();
                if (framework_quad) {
                    quad_layers.push_back((XrCompositionLayerBaseHeader*)&framework_quad->get());
                }
            }

            auto result = vr->m_openxr->end_frame(quad_layers, scene_depth_tex.Get() != nullptr);

            if (result == XR_ERROR_LAYER_INVALID) {
                spdlog::info("[VR] Attempting to correct invalid layer");

                m_openxr.wait_for_all_copies();

                spdlog::info("[VR] Calling xrEndFrame again");
                result = vr->m_openxr->end_frame(quad_layers);
            }

            vr->m_openxr->needs_pose_update = true;
            vr->m_submitted = result == XR_SUCCESS;
        }

        ////////////////////////////////////////////////////////////////////////////////
        // OpenVR start ////////////////////////////////////////////////////////////////
        ////////////////////////////////////////////////////////////////////////////////
        if (runtime->is_openvr()) {
            if (runtime->needs_pose_update) {
                vr->m_submitted = false;
                spdlog::info("[VR] Runtime needed pose update inside present (frame {})", vr->m_frame_count);
                return vr::VRCompositorError_None;
            }

            //++m_openvr.texture_counter;
        }

        // Allows the desktop window to be recorded.
        /*if (vr->m_desktop_fix->value()) {
            if (runtime->ready() && m_prev_backbuffer != backbuffer && m_prev_backbuffer != nullptr) {
                m_generic_commands[frame_count % 3].wait(INFINITE);
                m_generic_commands[frame_count % 3].copy(m_prev_backbuffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
                m_generic_commands[frame_count % 3].execute();
            }
        }*/
    }

    m_prev_backbuffer = backbuffer;

    return e;
}

void D3D12Component::log_frame_timing_stats_if_needed(VR* vr) {
    const auto now = std::chrono::steady_clock::now();

    if (m_last_frame_timing_log.time_since_epoch().count() == 0) {
        m_last_frame_timing_log = now;
        return;
    }

    if (now - m_last_frame_timing_log < FRAME_TIMING_LOG_INTERVAL) {
        return;
    }

    if (m_perf_on_frame.count == 0 &&
        m_perf_ui_copy.count == 0 &&
        m_perf_swapchain_copy.count == 0 &&
        m_perf_openxr_submit.count == 0 &&
        m_perf_spectator_mirror.count == 0 &&
        m_perf_post_present.count == 0)
    {
        m_last_frame_timing_log = now;
        return;
    }

    bool has_ui_target = false;
    bool ui_target_pending = false;
    uint32_t dedicated_ui_width = 0;
    uint32_t dedicated_ui_height = 0;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        const auto rtm = vr->m_fake_stereo_hook->get_render_target_manager();

        if (rtm != nullptr) {
            has_ui_target = rtm->get_ui_target() != nullptr;
            ui_target_pending = rtm->is_dedicated_ui_target_pending();
            dedicated_ui_width = rtm->get_dedicated_ui_width();
            dedicated_ui_height = rtm->get_dedicated_ui_height();
        }
    }

    const auto mirror_mode = vr != nullptr ? (int)vr->get_desktop_mirror_mode() : -1;
    const auto desktop_fix = vr != nullptr && vr->m_desktop_fix->value();
    const auto hmd_active = vr != nullptr && vr->is_hmd_active();
    const auto afr = vr != nullptr && vr->is_using_afr();
    const auto native_stereo = vr != nullptr && vr->is_native_stereo_fix_enabled();
    const auto has_ui_tex = m_game_ui_tex.texture.Get() != nullptr;
    const auto has_game_tex = m_game_tex.texture.Get() != nullptr;

    spdlog::info(
        "[D3D12][frame-profiler] on_frame avg={:.2f}ms max={:.2f}ms n={} ui_copy avg={:.2f}ms max={:.2f}ms n={} swapchain_copy avg={:.2f}ms max={:.2f}ms n={} openxr_submit avg={:.2f}ms max={:.2f}ms n={} spectator_mirror avg={:.2f}ms max={:.2f}ms n={} post_present avg={:.2f}ms max={:.2f}ms n={} mirror_mode={} desktop_fix={} hmd={} afr={} native_stereo={} has_game_tex={} has_ui_tex={} has_ui_target={} ui_pending={} ui_extent={}x{} submitted={} dbg_empty={} dbg_skip_scene={} dbg_skip_ui={} dbg_no_depth={}",
        m_perf_on_frame.avg(),
        m_perf_on_frame.max_ms,
        m_perf_on_frame.count,
        m_perf_ui_copy.avg(),
        m_perf_ui_copy.max_ms,
        m_perf_ui_copy.count,
        m_perf_swapchain_copy.avg(),
        m_perf_swapchain_copy.max_ms,
        m_perf_swapchain_copy.count,
        m_perf_openxr_submit.avg(),
        m_perf_openxr_submit.max_ms,
        m_perf_openxr_submit.count,
        m_perf_spectator_mirror.avg(),
        m_perf_spectator_mirror.max_ms,
        m_perf_spectator_mirror.count,
        m_perf_post_present.avg(),
        m_perf_post_present.max_ms,
        m_perf_post_present.count,
        mirror_mode,
        desktop_fix,
        hmd_active,
        afr,
        native_stereo,
        has_game_tex,
        has_ui_tex,
        has_ui_target,
        ui_target_pending,
        dedicated_ui_width,
        dedicated_ui_height,
        vr != nullptr && vr->m_submitted,
        vr != nullptr && vr->m_openxr != nullptr && vr->m_openxr->debug_submit_empty_frame->value(),
        vr != nullptr && vr->m_openxr != nullptr && vr->m_openxr->debug_skip_scene_copy->value(),
        vr != nullptr && vr->m_openxr != nullptr && vr->m_openxr->debug_skip_ui_copy->value(),
        vr != nullptr && vr->m_openxr != nullptr && vr->m_openxr->debug_disable_depth_submit->value()
    );

    m_last_frame_timing_log = now;
    m_perf_on_frame.reset();
    m_perf_ui_copy.reset();
    m_perf_swapchain_copy.reset();
    m_perf_openxr_submit.reset();
    m_perf_spectator_mirror.reset();
    m_perf_post_present.reset();
}

bool D3D12Component::has_game_and_ui_textures() const {
    return m_game_tex.texture.Get() != nullptr &&
        m_game_ui_tex.texture.Get() != nullptr;
}

D3D12Component::HitchFrameSnapshot D3D12Component::get_hitch_frame_snapshot(VR* vr) const {
    HitchFrameSnapshot snapshot{};
    snapshot.initialized = is_initialized();
    snapshot.force_reset = m_force_reset;
    snapshot.last_afr_state = m_last_afr_state;
    snapshot.has_prev_backbuffer = m_prev_backbuffer.Get() != nullptr;
    snapshot.has_game_tex = m_game_tex.texture.Get() != nullptr;
    snapshot.has_ui_tex = m_game_ui_tex.texture.Get() != nullptr;
    snapshot.has_scene_capture_tex = m_scene_capture_tex.texture.Get() != nullptr;
    snapshot.backbuffer_width = m_backbuffer_size[0];
    snapshot.backbuffer_height = m_backbuffer_size[1];
    const auto [ui_width, ui_height] = get_ui_extent();
    snapshot.ui_extent_width = ui_width;
    snapshot.ui_extent_height = ui_height;
    snapshot.hmd_width = vr != nullptr ? vr->get_hmd_width() : 0;
    snapshot.hmd_height = vr != nullptr ? vr->get_hmd_height() : 0;
    snapshot.swapchain_recreate_count = m_swapchain_recreate_count;
    snapshot.last_swapchain_recreate_reasons = m_last_swapchain_recreate_reasons;
    snapshot.perf_on_frame_count = m_perf_on_frame.count;
    snapshot.perf_on_frame_avg_ms = m_perf_on_frame.avg();
    snapshot.perf_on_frame_max_ms = m_perf_on_frame.max_ms;
    snapshot.perf_ui_copy_count = m_perf_ui_copy.count;
    snapshot.perf_ui_copy_avg_ms = m_perf_ui_copy.avg();
    snapshot.perf_ui_copy_max_ms = m_perf_ui_copy.max_ms;
    snapshot.perf_swapchain_copy_count = m_perf_swapchain_copy.count;
    snapshot.perf_swapchain_copy_avg_ms = m_perf_swapchain_copy.avg();
    snapshot.perf_swapchain_copy_max_ms = m_perf_swapchain_copy.max_ms;
    snapshot.perf_openxr_submit_count = m_perf_openxr_submit.count;
    snapshot.perf_openxr_submit_avg_ms = m_perf_openxr_submit.avg();
    snapshot.perf_openxr_submit_max_ms = m_perf_openxr_submit.max_ms;

    if (vr != nullptr && vr->m_openxr != nullptr) {
        const auto cached = vr->m_openxr->get_cached_swapchain_dimensions();
        snapshot.openxr_swapchain_count = cached.count;
        snapshot.ui_swapchain_width = cached.ui_width;
        snapshot.ui_swapchain_height = cached.ui_height;
        snapshot.eye_swapchain_width = cached.eye_width;
        snapshot.eye_swapchain_height = cached.eye_height;
        snapshot.depth_swapchain_width = cached.depth_width;
        snapshot.depth_swapchain_height = cached.depth_height;
    }

    return snapshot;
}

void D3D12Component::log_openxr_swapchain_recreate(VR* vr, uint32_t reasons, uint32_t new_depth_width, uint32_t new_depth_height) {
    if (reasons == SWAPCHAIN_RECREATE_NONE || vr == nullptr || vr->m_openxr == nullptr) {
        return;
    }

    uint32_t old_ui_width = 0;
    uint32_t old_ui_height = 0;
    uint32_t old_depth_width = 0;
    uint32_t old_depth_height = 0;
    uint32_t old_eye_width = 0;
    uint32_t old_eye_height = 0;
    size_t swapchain_count = 0;

    {
        std::scoped_lock _{vr->m_openxr->swapchain_mtx};
        swapchain_count = vr->m_openxr->swapchains.size();

        const auto read_swapchain = [&](runtimes::OpenXR::SwapchainIndex index, uint32_t& width, uint32_t& height) {
            const auto it = vr->m_openxr->swapchains.find((uint32_t)index);

            if (it != vr->m_openxr->swapchains.end()) {
                width = (uint32_t)std::max(0, it->second.width);
                height = (uint32_t)std::max(0, it->second.height);
            }
        };

        read_swapchain(runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, old_eye_width, old_eye_height);
        if (old_eye_width == 0 || old_eye_height == 0) {
            read_swapchain(runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, old_eye_width, old_eye_height);
        }
        read_swapchain(runtimes::OpenXR::SwapchainIndex::UI, old_ui_width, old_ui_height);
        read_swapchain(runtimes::OpenXR::SwapchainIndex::DEPTH, old_depth_width, old_depth_height);
        if (old_depth_width == 0 || old_depth_height == 0) {
            read_swapchain(runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, old_depth_width, old_depth_height);
        }
    }

    const auto [new_ui_width, new_ui_height] = get_ui_extent();
    ++m_swapchain_recreate_count;
    m_last_swapchain_recreate_reasons = reasons;

    SPDLOG_INFO(
        "[OpenXR][swapchain-recreate] reasons={} old_hmd={}x{} new_hmd={}x{} old_eye={}x{} old_ui={}x{} new_ui={}x{} old_depth={}x{} new_depth={}x{} old_afr={} new_afr={} swapchains={}",
        format_swapchain_recreate_reasons(reasons),
        m_openxr.last_resolution[0],
        m_openxr.last_resolution[1],
        vr->get_hmd_width(),
        vr->get_hmd_height(),
        old_eye_width,
        old_eye_height,
        old_ui_width,
        old_ui_height,
        new_ui_width,
        new_ui_height,
        old_depth_width,
        old_depth_height,
        new_depth_width,
        new_depth_height,
        m_last_afr_state,
        vr->is_using_afr(),
        swapchain_count);
}

std::unique_ptr<DirectX::DX12::SpriteBatch> D3D12Component::setup_sprite_batch_pso(
    DXGI_FORMAT output_format, 
    std::span<const uint8_t> ps, 
    std::span<const uint8_t> vs, 
    std::optional<DirectX::SpriteBatchPipelineStateDescription> pd) 
{
    spdlog::info("[D3D12] Setting up sprite batch PSO");

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    DirectX::ResourceUploadBatch upload{ device };
    upload.Begin();

    if (!pd) {
        pd = DirectX::SpriteBatchPipelineStateDescription{DirectX::RenderTargetState{output_format, DXGI_FORMAT_UNKNOWN}};
    }

    if (ps.size() > 0) {
        pd->customPixelShader = D3D12_SHADER_BYTECODE{ps.data(), ps.size()};
    }

    if (vs.size() > 0) {
        pd->customVertexShader = D3D12_SHADER_BYTECODE{vs.data(), vs.size()};
    }

    auto batch = std::make_unique<DirectX::DX12::SpriteBatch>(device, upload, *pd);

    auto result = upload.End(command_queue);
    result.wait();

    spdlog::info("[D3D12] Sprite batch PSO setup complete");

    return batch;
}

void D3D12Component::draw_spectator_view(ID3D12GraphicsCommandList* command_list, bool is_right_eye_frame, d3d12::TextureContext* game_tex_override) {
    if (command_list == nullptr) {
        SPDLOG_INFO_EVERY_N_SEC(5, "[D3D12][spectator] disabled: command list is null");
        return;
    }

    if (m_skip_spectator_view_for_volatile_external_rt) {
        SPDLOG_INFO_EVERY_N_SEC(2, "[SHf][D3D12] Skipping desktop mirror for volatile external RT");
        return;
    }

    const auto& vr = VR::get();
    const auto mirror_mode = vr->get_desktop_mirror_mode();
    const auto has_ui_tex = m_game_ui_tex.texture != nullptr && m_game_ui_tex.srv_heap != nullptr && m_game_ui_tex.srv_heap->Heap() != nullptr;

    if (!vr->is_hmd_active()) {
        SPDLOG_INFO_EVERY_N_SEC(
            5,
            "[D3D12][spectator] disabled: HMD inactive mirror_mode={} desktop_fix={} has_ui_tex={}",
            (int)mirror_mode,
            vr->m_desktop_fix->value(),
            has_ui_tex);
        return;
    }

    if (!vr->m_desktop_fix->value()) {
        SPDLOG_INFO_EVERY_N_SEC(
            5,
            "[D3D12][spectator] disabled: Desktop Spectator View is off mirror_mode={} has_ui_tex={} right_eye_frame={}",
            (int)mirror_mode,
            has_ui_tex,
            is_right_eye_frame);
        return;
    }

    auto& game_tex = game_tex_override != nullptr ? *game_tex_override : m_game_tex;
    const auto has_game_tex = game_tex.texture != nullptr && game_tex.srv_heap != nullptr && game_tex.srv_heap->Heap() != nullptr;

    if (!has_game_tex) {
        SPDLOG_INFO_EVERY_N_SEC(
            5,
            "[D3D12][spectator] disabled: game texture context unavailable tex={} srv_heap={} srv={} mirror_mode={} desktop_fix={}",
            game_tex.texture.Get() != nullptr,
            game_tex.srv_heap != nullptr,
            game_tex.srv_heap != nullptr && game_tex.srv_heap->Heap() != nullptr,
            (int)mirror_mode,
            vr->m_desktop_fix->value());
        return;
    }

    const auto spectator_mirror_start = std::chrono::steady_clock::now();
    utility::ScopeGuard spectator_mirror_timing_guard{[&]() {
        m_perf_spectator_mirror.add(std::chrono::steady_clock::now() - spectator_mirror_start);
    }};

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    const auto desc = backbuffer->GetDesc();

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        spdlog::error("[VR] Backbuffer RTV heap is null (D3D12)");
        return;
    }

    // Copy the previous right eye frame to the left eye frame
    const auto prev_index = (index + m_backbuffer_textures.size() - 1) % m_backbuffer_textures.size();
    if (vr->is_using_afr() && !is_right_eye_frame && m_backbuffer_textures[prev_index]->texture != nullptr) {
        const auto& last_right_eye_buffer = m_backbuffer_textures[prev_index]->texture;

        if (backbuffer.Get() != last_right_eye_buffer.Get()) {
            m_generic_commands[index % 3].wait(INFINITE);
            m_generic_commands[index % 3].copy(last_right_eye_buffer.Get(), backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT);
            m_generic_commands[index % 3].execute();

            return;
        }
    }

    auto& batch = m_backbuffer_batch;

    D3D12_VIEWPORT viewport{};
    viewport.Width = (float)desc.Width;
    viewport.Height = (float)desc.Height;
    viewport.MaxDepth = 1.0f;
    
    batch->SetViewport(viewport);

    D3D12_RECT scissor_rect{};
    scissor_rect.left = 0;
    scissor_rect.top = 0;
    scissor_rect.right = (LONG)desc.Width;
    scissor_rect.bottom = (LONG)desc.Height;

    // Transition backbuffer to D3D12_RESOURCE_STATE_RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    render::D3D12Diagnostics::get().record_resource_barriers("VR::D3D12Component::draw_spectator_view/BackbufferToRT", 1, &barrier);
    command_list->ResourceBarrier(1, &barrier);

    // Set RTV to backbuffer
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_heaps[] = { backbuffer_ctx.get_rtv() };
    render::D3D12Diagnostics::get().record_rtv_bind("VR::D3D12Component::draw_spectator_view/BackbufferRT", 1, rtv_heaps, nullptr);
    command_list->OMSetRenderTargets(1, rtv_heaps, FALSE, nullptr);

    // Clear backbuffer
    const float bb_clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    command_list->ClearRenderTargetView(backbuffer_ctx.get_rtv(), bb_clear_color, 0, nullptr);

    // Setup viewport and scissor rects
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor_rect);

    batch->Begin(command_list, DirectX::DX12::SpriteSortMode::SpriteSortMode_Immediate);

    RECT dest_rect{ 0, 0, (LONG)desc.Width, (LONG)desc.Height };

    const auto aspect_ratio = (float)desc.Width / (float)desc.Height;

    const auto eye_width = ((float)m_backbuffer_size[0] / 2.0f);
    const auto eye_height = (float)m_backbuffer_size[1];
    const auto eye_aspect_ratio = eye_width / eye_height;

    const auto original_centerw = (float)eye_width / 2.0f;
    const auto original_centerh = (float)eye_height / 2.0f;

    ///////////////
    // Eye (game) texture
    ///////////////
    // only show one half of the double wide texture (right side)
    RECT source_rect{};

    // Show left side when using AFR or native stereo fix
    if (vr->is_using_afr() || vr->is_native_stereo_fix_enabled()) {
        source_rect.left = 0;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0] / 2;
        source_rect.bottom = m_backbuffer_size[1];
    } else {
        source_rect.left = (LONG)m_backbuffer_size[0] / 2;
        source_rect.top = 0;
        source_rect.right = m_backbuffer_size[0];
        source_rect.bottom = m_backbuffer_size[1];
    }

    // Correct left/top/right/bottom to match the aspect ratio of the game
    if (eye_aspect_ratio > aspect_ratio) {
        const auto new_width = eye_height * aspect_ratio;
        const auto new_centerw = new_width / 2.0f;
        source_rect.left = (LONG)(original_centerw - new_centerw);
        source_rect.right = (LONG)(original_centerw + new_centerw);
    } else {
        const auto new_height = eye_width / aspect_ratio;
        const auto new_centerh = new_height / 2.0f;
        source_rect.top = (LONG)(original_centerh - new_centerh);
        source_rect.bottom = (LONG)(original_centerh + new_centerh);
    }

    // Set descriptor heaps
    ID3D12DescriptorHeap* game_heaps[] = { game_tex.srv_heap->Heap() };
    render::D3D12Diagnostics::get().record_descriptor_heaps_set("VR::D3D12Component::draw_spectator_view/GameSRV", 1, game_heaps);
    command_list->SetDescriptorHeaps(1, game_heaps);

    batch->Draw(game_tex.get_srv_gpu(),
        DirectX::XMUINT2{ (uint32_t)m_backbuffer_size[0], (uint32_t)m_backbuffer_size[1] },
        dest_rect,
        &source_rect, 
        DirectX::Colors::White);

    if (mirror_mode == VR::DESKTOP_MIRROR_FULL && has_ui_tex) {
        const auto ui_desc = m_game_ui_tex.texture->GetDesc();
        ID3D12DescriptorHeap* ui_heaps[] = { m_game_ui_tex.srv_heap->Heap() };
        render::D3D12Diagnostics::get().record_descriptor_heaps_set("VR::D3D12Component::draw_spectator_view/UISRV", 1, ui_heaps);
        command_list->SetDescriptorHeaps(1, ui_heaps);

        batch->Draw(m_game_ui_tex.get_srv_gpu(), 
            DirectX::XMUINT2{ (uint32_t)ui_desc.Width, (uint32_t)ui_desc.Height },
            dest_rect, 
            DirectX::Colors::White);
    }

    batch->End();

    // Transition backbuffer to D3D12_RESOURCE_STATE_PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    render::D3D12Diagnostics::get().record_resource_barriers("VR::D3D12Component::draw_spectator_view/BackbufferToPresent", 1, &barrier);
    command_list->ResourceBarrier(1, &barrier);
}

void D3D12Component::clear_backbuffer() {
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    if (device == nullptr || swapchain == nullptr) {
        return;
    }

    ComPtr<ID3D12Resource> backbuffer{};
    const auto index = swapchain->GetCurrentBackBufferIndex();

    if (FAILED(swapchain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        return;
    }

    if (backbuffer == nullptr) {
        return;
    }

    if (index >= m_backbuffer_textures.size()) {
        m_backbuffer_textures.resize(index + 1);
        spdlog::info("[VR] Resized backbuffer textures to {}", index + 1);

        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx_ptr = m_backbuffer_textures[index];
    
    if (backbuffer_ctx_ptr == nullptr) {
        // if this has happened, assume the rest of the textures are also null
        for (auto& tex : m_backbuffer_textures) {
            if (tex == nullptr) {
                tex = std::make_unique<d3d12::TextureContext>();
            }
        }
    }

    auto& backbuffer_ctx = *backbuffer_ctx_ptr;

    if (backbuffer_ctx.texture.Get() != backbuffer.Get()) {
        if (!backbuffer_ctx.setup(device, backbuffer.Get(), std::nullopt, std::nullopt, L"Backbuffer")) {
            spdlog::error("[VR] Failed to setup backbuffer RTV (D3D12)");
            return;
        }

        spdlog::info("[VR] Created backbuffer RTV (D3D12)");
    }

    // oh well
    if (backbuffer_ctx.rtv_heap == nullptr || backbuffer_ctx.rtv_heap->Heap() == nullptr) {
        return;
    }

    // Clear the backbuffer
    backbuffer_ctx.commands.wait(0);
    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    backbuffer_ctx.commands.clear_rtv(backbuffer_ctx.texture.Get(), backbuffer_ctx.get_rtv(), clear_color, D3D12_RESOURCE_STATE_PRESENT);
    backbuffer_ctx.commands.execute();
}

void D3D12Component::on_post_present(VR* vr) {
    const auto post_present_start = std::chrono::steady_clock::now();
    utility::ScopeGuard post_present_timing_guard{[&]() {
        m_perf_post_present.add(std::chrono::steady_clock::now() - post_present_start);
    }};

    if (m_graphics_memory != nullptr) {
        auto& hook = g_framework->get_d3d12_hook();

        auto device = hook->get_device();
        auto command_queue = hook->get_command_queue();

        m_graphics_memory->Commit(command_queue);
    }

    // Clear the (real) backbuffer if VR is enabled. Otherwise it will flicker and all sorts of nasty things.
    if (vr->is_hmd_active()) {
        clear_backbuffer();
    }
}

void D3D12Component::on_reset(VR* vr) {
    m_force_reset = true;
    m_last_frame_timing_log = {};
    m_perf_on_frame.reset();
    m_perf_ui_copy.reset();
    m_perf_swapchain_copy.reset();
    m_perf_openxr_submit.reset();
    m_perf_spectator_mirror.reset();
    m_perf_post_present.reset();

    auto runtime = vr->get_runtime();

    for (auto& ctx : m_openvr.left_eye_tex) {
        ctx.reset();
    }

    for (auto& ctx : m_openvr.right_eye_tex) {
        ctx.reset();
    }

    for (auto& commands : m_generic_commands) {
        commands.reset();
    }

    for (auto& commands : m_game_tex_commands) {
        commands.reset();
    }

    for (auto& backbuffer : m_backbuffer_textures) {
        backbuffer.reset();
    }

    for (auto & screen : m_2d_screen_tex) {
        screen.reset();
    }

    m_openvr.ui_tex.reset();
    m_game_ui_tex.reset();
    m_game_tex.reset();
    m_scene_capture_tex.reset();
    m_shf_mono_scene_tex.reset();
    m_shf_mono_scene_commands.reset();
    m_shf_mono_scene_width = 0;
    m_shf_mono_scene_height = 0;
    m_shf_mono_scene_format = DXGI_FORMAT_UNKNOWN;
    m_skip_spectator_view_for_volatile_external_rt = false;
    m_shf_scene_mode = ShfSceneMode::Unknown;
    m_backbuffer_batch.reset();
    m_game_batch.reset();
    m_ui_batch_alpha_invert.reset();
    m_graphics_memory.reset();

    if (runtime->is_openxr() && runtime->loaded) {
        m_openxr.wait_for_all_copies();

        auto& rt_pool = vr->get_render_target_pool_hook();
        ComPtr<ID3D12Resource> scene_depth_tex{rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ")};

        bool needs_depth_resize = false;

        if (scene_depth_tex != nullptr) {
            const auto desc = scene_depth_tex->GetDesc();
            needs_depth_resize = vr->m_openxr->needs_depth_resize(desc.Width, desc.Height);

            if (needs_depth_resize) {
                spdlog::info("[VR] SceneDepthZ needs resize ({}x{})", desc.Width, desc.Height);
            }
        }


        const auto [ui_width, ui_height] = get_ui_extent();
        uint32_t reasons = SWAPCHAIN_RECREATE_NONE;
        int32_t old_ui_width = 0;
        int32_t old_ui_height = 0;
        bool swapchains_empty = false;

        {
            std::scoped_lock _{vr->m_openxr->swapchain_mtx};
            swapchains_empty = vr->m_openxr->swapchains.empty();
            const auto ui_it = vr->m_openxr->swapchains.find((uint32_t)runtimes::OpenXR::SwapchainIndex::UI);

            if (ui_it != vr->m_openxr->swapchains.end()) {
                old_ui_width = ui_it->second.width;
                old_ui_height = ui_it->second.height;
            }
        }

        if (m_openxr.last_resolution[0] != vr->get_hmd_width() || m_openxr.last_resolution[1] != vr->get_hmd_height()) {
            reasons |= SWAPCHAIN_RECREATE_HMD_RESOLUTION;
        }

        if (swapchains_empty) {
            reasons |= SWAPCHAIN_RECREATE_EMPTY;
        } else if ((uint32_t)old_ui_width != ui_width || (uint32_t)old_ui_height != ui_height) {
            reasons |= SWAPCHAIN_RECREATE_UI_EXTENT;
        }

        if (m_last_afr_state != vr->is_using_afr()) {
            reasons |= SWAPCHAIN_RECREATE_AFR_STATE;
        }

        if (needs_depth_resize) {
            reasons |= SWAPCHAIN_RECREATE_DEPTH_EXTENT;
        }

        if (reasons != SWAPCHAIN_RECREATE_NONE) {
            uint32_t new_depth_width = 0;
            uint32_t new_depth_height = 0;

            if (scene_depth_tex != nullptr) {
                const auto desc = scene_depth_tex->GetDesc();
                new_depth_width = (uint32_t)desc.Width;
                new_depth_height = (uint32_t)desc.Height;
            }

            log_openxr_swapchain_recreate(vr, reasons, new_depth_width, new_depth_height);
            prepare_openxr_swapchain_recreate(vr, reasons);
            m_openxr.create_swapchains();
            m_last_afr_state = vr->is_using_afr();
        }

        // end the frame before something terrible happens
        //vr->m_openxr.synchronize_frame();
        //vr->m_openxr.begin_frame();
        //vr->m_openxr.end_frame();
    }

    m_prev_backbuffer.Reset();
    m_openvr.texture_counter = 0;
}

bool D3D12Component::setup() {
    SPDLOG_INFO_EVERY_N_SEC(1, "[VR] Setting up d3d12 textures...");

    auto vr = VR::get();
    on_reset(vr.get());
    
    m_prev_backbuffer.Reset();

    auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

    if (ue4_texture != nullptr) {
        backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
    }

    ComPtr<ID3D12Resource> real_backbuffer{};
    if (FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&real_backbuffer)))) {
        spdlog::error("[VR] Failed to get real back buffer (D3D12).");
        return false;
    }

    if (vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer = real_backbuffer;
    }

    if (backbuffer == nullptr) {
        SPDLOG_ERROR_EVERY_N_SEC(1, "[VR] Failed to get back buffer (D3D12).");
        return false;
    }

    if (m_graphics_memory == nullptr) {
        m_graphics_memory = std::make_unique<DirectX::DX12::GraphicsMemory>(device);
    }

    const auto real_backbuffer_desc = real_backbuffer->GetDesc();

    auto backbuffer_desc = backbuffer->GetDesc();

    spdlog::info("[VR] D3D12 Real backbuffer width: {}, height: {}, format: {}", real_backbuffer_desc.Width, real_backbuffer_desc.Height, (uint32_t)real_backbuffer_desc.Format);

    backbuffer_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    backbuffer_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    backbuffer_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        backbuffer_desc.Width /= 2; // The texture we get from UE is both eyes combined. we will copy the regions later.
    }

    spdlog::info("[VR] D3D12 RT width: {}, height: {}, format: {}", backbuffer_desc.Width, backbuffer_desc.Height, (uint32_t)backbuffer_desc.Format);

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    if (vr->is_using_2d_screen()) {
        ensure_2d_screen_textures(device, backbuffer_desc);
    }

    if (vr->get_runtime()->is_openvr()) {
        for (auto& ctx : m_openvr.left_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create left eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Left Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Left Eye")) {
                spdlog::error("[VR] Failed to setup left eye context.");
                return false;
            }
        }

        for (auto& ctx : m_openvr.right_eye_tex) {
            if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &backbuffer_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&ctx.texture)))) {
                spdlog::error("[VR] Failed to create right eye texture.");
                return false;
            }

            ctx.texture->SetName(L"OpenVR Right Eye Texture");
            if (!ctx.commands.setup(L"OpenVR Right Eye")) {
                spdlog::error("[VR] Failed to setup right eye context.");
                return false;
            }
        }

        // Set up the UI texture to match the engine-provided UI extent when available.
        auto ui_desc = backbuffer_desc;
        const auto [ui_width, ui_height] = get_ui_extent();
        ui_desc.Width = ui_width;
        ui_desc.Height = ui_height;

        ComPtr<ID3D12Resource> ui_tex{};
        if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &ui_desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&ui_tex)))) {
            spdlog::error("[VR] Failed to create UI texture.");
            return false;
        }

        ui_tex->SetName(L"OpenVR UI Texture");

        if (!m_openvr.ui_tex.setup(device, ui_tex.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, L"OpenVR UI")) {
            spdlog::error("[VR] Failed to setup OpenVR UI context.");
            return false;
        }
    }

    for (auto& commands : m_generic_commands) {
        if (!commands.setup(L"Generic commands")) {
            return false;
        }
    }

    if (!vr->is_extreme_compatibility_mode_enabled()) {
        m_backbuffer_size[0] = backbuffer_desc.Width * 2;
    } else {
        m_backbuffer_size[0] = backbuffer_desc.Width;
    }

    m_backbuffer_size[1] = backbuffer_desc.Height;

    m_backbuffer_batch = setup_sprite_batch_pso(real_backbuffer_desc.Format);
    m_game_batch = setup_sprite_batch_pso(backbuffer_desc.Format);

    // Custom blend state to flip the alpha in-place of the UI texture without an intermediate render target
    {
        DirectX::SpriteBatchPipelineStateDescription invert_alpha_in_place_pd{DirectX::RenderTargetState{backbuffer_desc.Format, DXGI_FORMAT_UNKNOWN}};

        auto& bd = invert_alpha_in_place_pd.blendDesc;
        auto& bdrt = bd.RenderTarget[0];
        bdrt.BlendEnable = TRUE;

        bdrt.SrcBlend = D3D12_BLEND_ONE;
        bdrt.DestBlend = D3D12_BLEND_ZERO;
        bdrt.BlendOp = D3D12_BLEND_OP_ADD;

        bdrt.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        bdrt.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        bdrt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bdrt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        m_ui_batch_alpha_invert = setup_sprite_batch_pso(
            backbuffer_desc.Format, 
            alpha_luminance_sprite_ps_SpritePixelShader, 
            alpha_luminance_sprite_ps_SpriteVertexShader, 
            invert_alpha_in_place_pd
        );
    }

    spdlog::info("[VR] d3d12 textures have been setup");
    m_force_reset = false;

    return true;
}

void D3D12Component::OpenXR::initialize(XrSessionCreateInfo& session_info) {
    std::scoped_lock _{this->mtx};

	auto& hook = g_framework->get_d3d12_hook();

    auto device = hook->get_device();
    auto command_queue = hook->get_command_queue();

    this->binding.device = device;
    this->binding.queue = command_queue;

    spdlog::info("[VR] Searching for xrGetD3D12GraphicsRequirementsKHR...");
    PFN_xrGetD3D12GraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(VR::get()->m_openxr->instance, "xrGetD3D12GraphicsRequirementsKHR", (PFN_xrVoidFunction*)(&fn));

    XrGraphicsRequirementsD3D12KHR gr{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    gr.adapterLuid = device->GetAdapterLuid();
    gr.minFeatureLevel = D3D_FEATURE_LEVEL_11_0;

    spdlog::info("[VR] Calling xrGetD3D12GraphicsRequirementsKHR");
    fn(VR::get()->m_openxr->instance, VR::get()->m_openxr->system, &gr);

    session_info.next = &this->binding;
}

std::optional<std::string> D3D12Component::OpenXR::create_swapchains() {
    std::scoped_lock _{this->mtx};

    spdlog::info("[VR] Creating OpenXR swapchains for D3D12");

    this->destroy_swapchains();
    
    auto& hook = g_framework->get_d3d12_hook();
    auto device = hook->get_device();
    auto swapchain = hook->get_swap_chain();

    ComPtr<ID3D12Resource> backbuffer{};

    auto vr = VR::get();
    bool has_actual_vr_backbuffer = false;

    if (vr != nullptr && vr->m_fake_stereo_hook != nullptr) {
        auto ue4_texture = vr->m_fake_stereo_hook->get_render_target_manager()->get_render_target();

        if (ue4_texture != nullptr) {
            backbuffer = (ID3D12Resource*)ue4_texture->get_native_resource();
            has_actual_vr_backbuffer = backbuffer != nullptr;
        }
    }
    
    // Get the existing backbuffer
    // so we can get the format and stuff.
    if (backbuffer == nullptr && FAILED(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)))) {
        spdlog::error("[VR] Failed to get back buffer.");
        return "Failed to get back buffer.";
    }

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    auto backbuffer_desc = backbuffer->GetDesc();
    auto& openxr = vr->m_openxr;

    this->contexts.clear();

    auto create_swapchain = [&](uint32_t i, const XrSwapchainCreateInfo& swapchain_create_info, const D3D12_RESOURCE_DESC& desc) -> std::optional<std::string> {
        // Create the swapchain.
        runtimes::OpenXR::Swapchain swapchain{};
        swapchain.width = swapchain_create_info.width;
        swapchain.height = swapchain_create_info.height;

        if (xrCreateSwapchain(openxr->session, &swapchain_create_info, &swapchain.handle) != XR_SUCCESS) {
            spdlog::error("[VR] D3D12: Failed to create swapchain.");
            return "Failed to create swapchain.";
        }

        vr->m_openxr->swapchains[i] = swapchain;
        vr->m_openxr->cache_swapchain_dimensions(i, swapchain.width, swapchain.height);

        uint32_t image_count{};
        auto result = xrEnumerateSwapchainImages(swapchain.handle, 0, &image_count, nullptr);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images.");
            return "Failed to enumerate swapchain images.";
        }

        SPDLOG_INFO("[VR] Runtime wants {} images for swapchain {}", image_count, i);

        auto& ctx = this->contexts[i];

        ctx.textures.clear();
        ctx.textures.resize(image_count);
        ctx.texture_contexts.clear();
        ctx.texture_contexts.resize(image_count);

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j] = {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
            ctx.texture_contexts[j] = std::make_unique<d3d12::TextureContext>();
            ctx.texture_contexts[j]->commands.setup((std::wstring{L"OpenXR commands "} + std::to_wstring(i) + L" " + std::to_wstring(j)).c_str());
        }

        result = xrEnumerateSwapchainImages(swapchain.handle, image_count, &image_count, (XrSwapchainImageBaseHeader*)&ctx.textures[0]);
        
        if (result != XR_SUCCESS) {
            spdlog::error("[VR] Failed to enumerate swapchain images after texture creation.");
            return "Failed to enumerate swapchain images after texture creation.";
        }

        for (uint32_t j = 0; j < image_count; ++j) {
            ctx.textures[j].texture->AddRef();
            const auto ref_count = ctx.textures[j].texture->Release();

            spdlog::info("[VR] AFTER Swapchain texture {} {} ref count: {}", i, j, ref_count);
        }

        if (swapchain_create_info.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) {
            for (uint32_t j = 0; j < image_count; ++j) {
                XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wait_info.timeout = XR_INFINITE_DURATION;
                XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};

                uint32_t index{};
                xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &index);
                xrWaitSwapchainImage(swapchain.handle, &wait_info);

                auto& texture_ctx = ctx.texture_contexts[index];
                texture_ctx->texture = ctx.textures[index].texture;

                // Depth stencil textures don't need an RTV.
                if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
                    if (ctx.texture_contexts[index]->create_rtv(device, (DXGI_FORMAT)swapchain_create_info.format)) {
                        const float clear_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        texture_ctx->commands.clear_rtv(ctx.textures[index].texture, texture_ctx->get_rtv(), clear_color, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        texture_ctx->commands.execute();
                        texture_ctx->commands.wait(100);
                    } else {
                        spdlog::error("[VR] Failed to create RTV for swapchain image {}.", index);
                    }
                }

                texture_ctx->texture.Reset();
                texture_ctx->rtv_heap.reset();

                xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }
        }

        return std::nullopt;
    };

    const auto double_wide_multiple = vr->is_using_afr() ? 1 : 2;

    XrSwapchainCreateInfo standard_swapchain_create_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    standard_swapchain_create_info.arraySize = 1;
    standard_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    standard_swapchain_create_info.width = vr->get_hmd_width() * double_wide_multiple;
    standard_swapchain_create_info.height = vr->get_hmd_height();
    standard_swapchain_create_info.mipCount = 1;
    standard_swapchain_create_info.faceCount = 1;
    standard_swapchain_create_info.sampleCount = backbuffer_desc.SampleDesc.Count;
    standard_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;

    auto hmd_desc = backbuffer_desc;
    hmd_desc.Width = vr->get_hmd_width() * double_wide_multiple;
    hmd_desc.Height = vr->get_hmd_height();
    hmd_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    hmd_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    hmd_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // Above is outdated, we will just use a double wide texture
    if (!vr->is_using_afr()) {
        spdlog::info("[VR] Creating double wide swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width() * 2);
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DOUBLE_WIDE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    } else {
        spdlog::info("[VR] Creating AFR swapchain for eyes");
        spdlog::info("[VR] Width: {}", vr->get_hmd_width());
        spdlog::info("[VR] Height: {}", vr->get_hmd_height());

        spdlog::info("[VR] Creating AFR left eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_LEFT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }

        spdlog::info("[VR] Creating AFR right eye swapchain");
        if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_RIGHT_EYE, standard_swapchain_create_info, hmd_desc)) {
            return err;
        }
    }

    auto virtual_desktop_dummy_desc = backbuffer_desc;
    auto virtual_desktop_dummy_swapchain_create_info = standard_swapchain_create_info;

    virtual_desktop_dummy_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    virtual_desktop_dummy_desc.Width = 4;
    virtual_desktop_dummy_desc.Height = 4;
    virtual_desktop_dummy_swapchain_create_info.width = 4;
    virtual_desktop_dummy_swapchain_create_info.height = 4;
    virtual_desktop_dummy_swapchain_create_info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT; // so we dont need to acquire/release/wait

    // The virtual desktop dummy texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DUMMY_VIRTUAL_DESKTOP, virtual_desktop_dummy_swapchain_create_info, virtual_desktop_dummy_desc)) {
        return err;
    }

    const auto [ui_width, ui_height] = get_ui_extent();
    spdlog::info("[VR] OpenXR UI extent: {}x{}", ui_width, ui_height);

    auto desktop_rt_swapchain_create_info = standard_swapchain_create_info;
    desktop_rt_swapchain_create_info.format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_swapchain_create_info.width = ui_width;
    desktop_rt_swapchain_create_info.height = ui_height;

    auto desktop_rt_desc = backbuffer_desc;
    desktop_rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    desktop_rt_desc.Width = ui_width;
    desktop_rt_desc.Height = ui_height;

    desktop_rt_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    desktop_rt_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    // The UI texture
    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::UI_RIGHT, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::FRAMEWORK_UI, desktop_rt_swapchain_create_info, desktop_rt_desc)) {
        return err;
    }

    // Depth textures
    if (vr->get_openxr_runtime()->is_depth_allowed()) {
        // Even when using AFR, the depth tex is always the size of a double wide.
        // That's kind of unfortunate in terms of how many copies we have to do but whatever.
        auto depth_swapchain_create_info = standard_swapchain_create_info;
        depth_swapchain_create_info.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        depth_swapchain_create_info.createFlags = 0;
        depth_swapchain_create_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
        depth_swapchain_create_info.width = vr->get_hmd_width() * 2;
        depth_swapchain_create_info.height = vr->get_hmd_height();

        auto depth_desc = backbuffer_desc;
        depth_desc.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
        //depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_desc.DepthOrArraySize = 1;

        depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

        depth_desc.Width = vr->get_hmd_width() * 2;
        depth_desc.Height = vr->get_hmd_height();

        auto& rt_pool = vr->get_render_target_pool_hook();
        auto depth_tex = rt_pool->get_texture<ID3D12Resource>(L"SceneDepthZ");

        if (depth_tex != nullptr) {
            this->made_depth_with_null_defaults = false;
            depth_desc = depth_tex->GetDesc();

            if (depth_desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
                depth_swapchain_create_info.format = DXGI_FORMAT_D24_UNORM_S8_UINT;
            }

            spdlog::info("[VR] Depth texture size: {}x{}", depth_desc.Width, depth_desc.Height);
            spdlog::info("[VR] Depth texture format: {}", (uint32_t)depth_desc.Format);
            spdlog::info("[VR] Depth texture flags: {}", (uint32_t)depth_desc.Flags);

            if (depth_desc.Width > hmd_desc.Width || depth_desc.Height > hmd_desc.Height) {
                spdlog::info("[VR] Depth texture is larger than the HMD");
                //depth_desc.Width = hmd_desc.Width;
                //depth_desc.Height = hmd_desc.Height;
            }

            depth_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            depth_desc.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

            depth_swapchain_create_info.width = depth_desc.Width;
            depth_swapchain_create_info.height = depth_desc.Height;
        } else {
            this->made_depth_with_null_defaults = true;
            spdlog::error("[VR] Depth texture is null! Using default values");
            depth_desc.Width = vr->get_hmd_width() * 2;
            depth_desc.Height = vr->get_hmd_height();
        }

        if (!vr->is_using_afr()) {
            spdlog::info("[VR] Creating double wide depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::DEPTH, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        } else {
            spdlog::info("[VR] Creating AFR depth swapchain");
            spdlog::info("[VR] Creating AFR left eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_LEFT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }

            spdlog::info("[VR] Creating AFR right eye depth swapchain");
            if (auto err = create_swapchain((uint32_t)runtimes::OpenXR::SwapchainIndex::AFR_DEPTH_RIGHT_EYE, depth_swapchain_create_info, depth_desc)) {
                return err;
            }
        }
    }

    this->last_resolution = {vr->get_hmd_width(), vr->get_hmd_height()};

    return std::nullopt;
}

void D3D12Component::OpenXR::destroy_swapchains() {
    std::scoped_lock _{this->mtx};
    auto vr = VR::get();

    if (vr != nullptr && vr->m_openxr != nullptr) {
        vr->m_openxr->clear_cached_swapchain_dimensions();
    }

    if (this->contexts.empty()) {
        return;
    }

    if (vr == nullptr || vr->m_openxr == nullptr) {
        return;
    }
    
    std::scoped_lock __{vr->m_openxr->swapchain_mtx};

    spdlog::info("[VR] Destroying swapchains.");

    this->wait_for_all_copies();

    for (auto& it : this->contexts) {
        auto& ctx = it.second;
        const auto i = it.first;

        //ctx.texture_contexts.clear();
        for (auto& texture_context : ctx.texture_contexts) {
            if (texture_context != nullptr) {
                texture_context->reset();
            }
        }

        ctx.texture_contexts.clear();

        std::vector<ID3D12Resource*> needs_release{};

        for (auto& tex : ctx.textures) {
            if (tex.texture != nullptr) {
                tex.texture->AddRef();
                needs_release.push_back(tex.texture);
            }
        }

        if (vr->m_openxr->swapchains.contains(i)) {
            const auto result = xrDestroySwapchain(vr->m_openxr->swapchains[i].handle);

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] Failed to destroy swapchain {}.", i);
            } else {
                spdlog::info("[VR] Destroyed swapchain {}.", i);
            }
        } else {
            spdlog::error("[VR] Swapchain {} does not exist.", i);
        }

        for (auto& tex : needs_release) {
            if (const auto ref_count = tex->Release(); ref_count != 0) {
                spdlog::info("[VR] Memory leak detected in swapchain texture {} ({} refs)", i, ref_count);
            } else {
                spdlog::info("[VR] Swapchain texture {} released.", i);
            }
        }
        
        ctx.textures.clear();
    }

    this->contexts.clear();
    vr->m_openxr->swapchains.clear();
}

void D3D12Component::OpenXR::copy(
    uint32_t swapchain_idx, 
    ID3D12Resource* resource, 
    std::optional<std::function<void(d3d12::CommandContext&, ID3D12Resource*)>> pre_commands, 
    std::optional<std::function<void(d3d12::CommandContext&)>> additional_commands, 
    D3D12_RESOURCE_STATES src_state, 
    D3D12_BOX* src_box) 
{
    std::scoped_lock _{this->mtx};

    auto vr = VR::get();

    if (vr->m_openxr->frame_state.shouldRender != XR_TRUE) {
        return;
    }

    if (!vr->m_openxr->frame_began) {
        if (vr->get_synchronize_stage() != VR::SynchronizeStage::VERY_LATE) {
            spdlog::error("[VR] OpenXR: Frame not begun when trying to copy.");
            return;
        }
    }

    if (!this->contexts.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (!vr->m_openxr->swapchains.contains(swapchain_idx)) {
        spdlog::error("[VR] OpenXR: Trying to copy to swapchain {} but it doesn't exist.", swapchain_idx);
        return;
    }

    if (this->contexts[swapchain_idx].num_textures_acquired > 0) {
        spdlog::info("[VR] Already acquired textures for swapchain {}?", swapchain_idx);
    }

    const auto& swapchain = vr->m_openxr->swapchains[swapchain_idx];
    auto& ctx = this->contexts[swapchain_idx];

    XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

    uint32_t texture_index{};
    auto result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);

    if (result == XR_ERROR_RUNTIME_FAILURE) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        spdlog::info("[VR] Attempting to correct...");

        for (auto& texture_ctx : ctx.texture_contexts) {
            texture_ctx->commands.reset();
        }

        texture_index = 0;
        result = xrAcquireSwapchainImage(swapchain.handle, &acquire_info, &texture_index);
    }


    if (result != XR_SUCCESS) {
        spdlog::error("[VR] xrAcquireSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
    } else {
        ctx.num_textures_acquired++;

        XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        //wait_info.timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)).count();
        wait_info.timeout = XR_INFINITE_DURATION;
        result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

        if (result != XR_SUCCESS) {
            spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
        } else {
            auto& texture_ctx = ctx.texture_contexts[texture_index];
            texture_ctx->commands.wait(INFINITE);

            if (pre_commands) {
                (*pre_commands)(texture_ctx->commands, ctx.textures[texture_index].texture);
            }

            const auto is_depth = is_depth_swapchain(swapchain_idx);
            const auto dst_state = src_box == nullptr && is_depth
                ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                : D3D12_RESOURCE_STATE_RENDER_TARGET;

            auto& dx12_diagnostics = render::D3D12Diagnostics::get();
            if (dx12_diagnostics.is_enabled()) {
                dx12_diagnostics.record_texture_copy(
                    "D3D12Component::OpenXR::copy",
                    swapchain_idx,
                    openxr_swapchain_name(swapchain_idx),
                    resource,
                    ctx.textures[texture_index].texture,
                    src_state,
                    dst_state,
                    src_box,
                    is_scene_swapchain(swapchain_idx),
                    is_ui_swapchain(swapchain_idx),
                    is_depth
                );
            }

            // We may simply just want to render to the render target directly
            // hence, a null resource is allowed.
            if (resource != nullptr) {
                if (src_box == nullptr) {
                    texture_ctx->commands.copy(
                        resource, 
                        ctx.textures[texture_index].texture, 
                        src_state, 
                        dst_state);
                } else {
                    texture_ctx->commands.copy_region(
                        resource, 
                        ctx.textures[texture_index].texture, src_box,
                        src_state, 
                        D3D12_RESOURCE_STATE_RENDER_TARGET);
                }
            }

            if (additional_commands) {
                (*additional_commands)(texture_ctx->commands);
            }

            texture_ctx->commands.execute();

            XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            auto result = xrReleaseSwapchainImage(swapchain.handle, &release_info);

            // SteamVR shenanigans.
            if (result == XR_ERROR_RUNTIME_FAILURE) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                spdlog::info("[VR] Attempting to correct...");

                result = xrWaitSwapchainImage(swapchain.handle, &wait_info);

                if (result != XR_SUCCESS) {
                    spdlog::error("[VR] xrWaitSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                }

                for (auto& texture_ctx : ctx.texture_contexts) {
                    texture_ctx->commands.wait(INFINITE);
                }

                result = xrReleaseSwapchainImage(swapchain.handle, &release_info);
            }

            if (result != XR_SUCCESS) {
                spdlog::error("[VR] xrReleaseSwapchainImage failed: {}", vr->m_openxr->get_result_string(result));
                return;
            }

            ctx.num_textures_acquired--;
            ctx.ever_acquired = true;
        }
    }
}
} // namespace vrmod
