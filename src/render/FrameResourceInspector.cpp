#include "render/FrameResourceInspector.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>

#include "Framework.hpp"
#include "mods/VR.hpp"
#include "mods/vr/D3D11Component.hpp"
#include "mods/vr/D3D12Component.hpp"
#include "mods/vr/RenderTargetPoolHook.hpp"
#include "utility/String.hpp"

namespace {
constexpr uint64_t STALE_RESOURCE_FRAME_WINDOW = 1200;
constexpr uint64_t RECENT_RESOURCE_FRAME_WINDOW = 180;

std::string to_lower(std::string_view value) {
    std::string lowered{};
    lowered.reserve(value.size());

    for (const auto c : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    return lowered;
}

bool contains_icase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return false;
    }

    const auto lowered_haystack = to_lower(haystack);
    const auto lowered_needle = to_lower(needle);

    return lowered_haystack.find(lowered_needle) != std::string::npos;
}

void append_token(std::string& destination, std::string_view token, std::string_view separator = " | ") {
    if (token.empty()) {
        return;
    }

    if (destination.empty()) {
        destination = token;
        return;
    }

    if (destination.find(token) != std::string::npos) {
        return;
    }

    destination += separator;
    destination += token;
}

std::string format_pointer(uintptr_t pointer) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << pointer;
    return ss.str();
}

std::string format_resolution(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return "-";
    }

    return std::to_string(width) + "x" + std::to_string(height);
}

bool is_depth_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return true;
    default:
        return false;
    }
}

std::optional<DXGI_FORMAT> preview_format_for_texture(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return format;
    }
}

std::string dxgi_format_to_string(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_UNKNOWN: return "UNKNOWN";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT";
    case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
    case DXGI_FORMAT_D16_UNORM: return "D16_UNORM";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    default:
        return std::to_string(static_cast<uint32_t>(format));
    }
}

std::string guess_tags_from_name_and_source(std::string_view name, std::string_view source) {
    std::string tags{};

    if (contains_icase(name, "depth") || contains_icase(source, "Depth")) {
        append_token(tags, "Depth");
    }

    if (contains_icase(name, "ui") || contains_icase(name, "hud") || contains_icase(name, "slate") || contains_icase(source, "UI")) {
        append_token(tags, "UI");
    }

    if (contains_icase(name, "swapchain") || contains_icase(source, "Swapchain")) {
        append_token(tags, "Swapchain");
    }

    if (contains_icase(name, "eye") || contains_icase(name, "xr") || contains_icase(source, "Eye")) {
        append_token(tags, "Eye");
    }

    if (contains_icase(name, "velocity") || contains_icase(name, "motion")) {
        append_token(tags, "Velocity?");
    }

    if (contains_icase(source, "RT Pool")) {
        append_token(tags, "RT Pool");
    }

    if (contains_icase(source, "Framework")) {
        append_token(tags, "Framework");
    }

    if (contains_icase(source, "OpenXR") || contains_icase(source, "OpenVR") || contains_icase(name, "backbuffer") || contains_icase(name, "[") || contains_icase(source, "Swapchain")) {
        append_token(tags, "Transient");
    }

    return tags;
}

std::string guess_name_if_empty(std::string_view current_name, uintptr_t pointer) {
    if (!current_name.empty()) {
        return std::string{current_name};
    }

    return "Resource " + format_pointer(pointer);
}
} // namespace

namespace render {
uint64_t FrameResourceInspector::make_key(Backend backend, uintptr_t pointer) const {
    return (static_cast<uint64_t>(backend) << 60) ^ static_cast<uint64_t>(pointer);
}

void FrameResourceInspector::on_present(Framework& framework, VR& vr) {
    std::scoped_lock _{m_mutex};
    ++m_frame_index;
    m_live_d3d11_resources.clear();
    m_live_d3d12_resources.clear();

    if (framework.is_dx11()) {
        sample_d3d11(framework, vr);
    } else if (framework.is_dx12()) {
        sample_d3d12(framework, vr);
    }

    prune_stale_resources();
    refresh_selected_preview(framework);
}

std::vector<FrameResourceInspector::ResourceInfo> FrameResourceInspector::snapshot() const {
    std::scoped_lock _{m_mutex};

    std::vector<ResourceInfo> resources{};
    resources.reserve(m_resources.size());

    for (const auto& [_, resource] : m_resources) {
        auto copy = resource;
        copy.is_recent = (m_frame_index >= copy.last_seen_frame) && ((m_frame_index - copy.last_seen_frame) <= RECENT_RESOURCE_FRAME_WINDOW);
        resources.emplace_back(std::move(copy));
    }

    std::sort(resources.begin(), resources.end(), [](const auto& a, const auto& b) {
        if (a.last_seen_frame != b.last_seen_frame) {
            return a.last_seen_frame > b.last_seen_frame;
        }

        if (a.backend != b.backend) {
            return a.backend < b.backend;
        }

        return a.name < b.name;
    });

    return resources;
}

uint64_t FrameResourceInspector::current_frame() const {
    std::scoped_lock _{m_mutex};
    return m_frame_index;
}

void FrameResourceInspector::reset() {
    std::scoped_lock _{m_mutex};
    m_resources.clear();
    m_live_d3d11_resources.clear();
    m_live_d3d12_resources.clear();
    m_selected_key.reset();
    m_preview = {};
    m_d3d11_preview_srv.Reset();
    m_frame_index = 0;
}

void FrameResourceInspector::set_selected_resource(std::optional<uint64_t> key) {
    std::scoped_lock _{m_mutex};

    if (m_selected_key == key) {
        return;
    }

    m_selected_key = key;
    m_preview = {};
    m_d3d11_preview_srv.Reset();
}

std::optional<uint64_t> FrameResourceInspector::selected_resource() const {
    std::scoped_lock _{m_mutex};
    return m_selected_key;
}

FrameResourceInspector::PreviewInfo FrameResourceInspector::preview_info() const {
    std::scoped_lock _{m_mutex};
    return m_preview;
}

void FrameResourceInspector::prune_stale_resources() {
    for (auto it = m_resources.begin(); it != m_resources.end();) {
        const auto age = (m_frame_index >= it->second.last_seen_frame) ? (m_frame_index - it->second.last_seen_frame) : 0;

        if (age > STALE_RESOURCE_FRAME_WINDOW) {
            it = m_resources.erase(it);
        } else {
            ++it;
        }
    }
}

void FrameResourceInspector::merge_or_add(ResourceInfo incoming) {
    if (incoming.pointer == 0) {
        return;
    }

    incoming.name = guess_name_if_empty(incoming.name, incoming.pointer);
    incoming.resolution = format_resolution(incoming.width, incoming.height);

    const auto key = make_key(incoming.backend, incoming.pointer);
    incoming.key = key;
    auto it = m_resources.find(key);

    if (it == m_resources.end()) {
        incoming.first_seen_frame = m_frame_index;
        incoming.last_seen_frame = m_frame_index;
        incoming.seen_count = 1;
        m_resources.emplace(key, std::move(incoming));
        return;
    }

    auto& existing = it->second;
    const auto descriptor_changed =
        existing.width != incoming.width ||
        existing.height != incoming.height ||
        existing.format != incoming.format ||
        existing.type != incoming.type;

    if (descriptor_changed) {
        ++existing.change_count;
        existing.width = incoming.width;
        existing.height = incoming.height;
        existing.format = incoming.format;
        existing.type = incoming.type;
        existing.resolution = incoming.resolution;
    }

    if (incoming.name.size() > existing.name.size()) {
        existing.name = incoming.name;
    }

    append_token(existing.source, incoming.source);
    append_token(existing.tags, incoming.tags, ", ");

    existing.is_depth = existing.is_depth || incoming.is_depth;
    existing.is_render_target = existing.is_render_target || incoming.is_render_target;
    existing.is_ui = existing.is_ui || incoming.is_ui;
    existing.is_swapchain = existing.is_swapchain || incoming.is_swapchain;
    existing.is_eye = existing.is_eye || incoming.is_eye;
    existing.is_velocity_candidate = existing.is_velocity_candidate || incoming.is_velocity_candidate;
    existing.is_rt_pool = existing.is_rt_pool || incoming.is_rt_pool;
    existing.is_transient = existing.is_transient || incoming.is_transient;

    existing.last_seen_frame = m_frame_index;
    ++existing.seen_count;
}

void FrameResourceInspector::refresh_selected_preview(Framework& framework) {
    m_preview = {};

    if (!m_selected_key) {
        return;
    }

    const auto resource_it = m_resources.find(*m_selected_key);
    if (resource_it == m_resources.end()) {
        m_preview.has_selection = true;
        m_preview.resource_key = *m_selected_key;
        m_preview.status = "Selected resource is no longer tracked";
        return;
    }

    const auto& resource = resource_it->second;
    m_preview.has_selection = true;
    m_preview.resource_key = *m_selected_key;
    m_preview.backend = resource.backend;
    m_preview.width = resource.width;
    m_preview.height = resource.height;
    m_preview.format = resource.format;

    std::optional<PreviewInfo> preview{};

    if (resource.backend == Backend::D3D11) {
        preview = create_d3d11_preview(framework, *m_selected_key);
    } else {
        preview = create_d3d12_preview(framework, *m_selected_key);
    }

    if (preview) {
        m_preview = *preview;
    } else {
        m_preview.status = "Preview unavailable";
    }
}

void FrameResourceInspector::track_d3d11_resource(
    const ID3D11Resource* resource,
    const std::string& name,
    const std::string& source,
    const std::string& extra_tags
) {
    if (resource == nullptr) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};

    if (FAILED(const_cast<ID3D11Resource*>(resource)->QueryInterface(IID_PPV_ARGS(&texture)))) {
        return;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    ResourceInfo info{};
    info.backend = Backend::D3D11;
    info.pointer = reinterpret_cast<uintptr_t>(texture.Get());
    info.name = name;
    info.source = source;
    info.type = desc.ArraySize > 1 ? "Texture2DArray" : "Texture2D";
    info.format = dxgi_format_to_string(desc.Format);
    info.width = desc.Width;
    info.height = desc.Height;
    info.tags = guess_tags_from_name_and_source(info.name, info.source);
    info.is_depth = is_depth_format(desc.Format) || (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0;
    info.is_render_target = (desc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0;
    info.is_ui = contains_icase(info.tags, "UI");
    info.is_swapchain = contains_icase(info.tags, "Swapchain");
    info.is_eye = contains_icase(info.tags, "Eye");
    info.is_velocity_candidate = contains_icase(info.tags, "Velocity?");
    info.is_rt_pool = contains_icase(info.tags, "RT Pool");

    if (info.is_depth) {
        append_token(info.tags, "Depth", ", ");
    }

    if (info.is_render_target) {
        append_token(info.tags, "RT", ", ");
    }

    append_token(info.tags, extra_tags, ", ");
    info.is_transient = contains_icase(info.tags, "Transient");
    info.key = make_key(info.backend, info.pointer);
    m_live_d3d11_resources[info.key] = texture;

    merge_or_add(std::move(info));
}

void FrameResourceInspector::track_d3d12_resource(
    ID3D12Resource* resource,
    const std::string& name,
    const std::string& source,
    const std::string& extra_tags
) {
    if (resource == nullptr) {
        return;
    }

    const auto desc = resource->GetDesc();

    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return;
    }

    ResourceInfo info{};
    info.backend = Backend::D3D12;
    info.pointer = reinterpret_cast<uintptr_t>(resource);
    info.name = name;
    info.source = source;
    info.type = desc.DepthOrArraySize > 1 ? "Texture2DArray" : "Texture2D";
    info.format = dxgi_format_to_string(desc.Format);
    info.width = static_cast<uint32_t>(desc.Width);
    info.height = desc.Height;
    info.tags = guess_tags_from_name_and_source(info.name, info.source);
    info.is_depth = is_depth_format(desc.Format) || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    info.is_render_target = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
    info.is_ui = contains_icase(info.tags, "UI");
    info.is_swapchain = contains_icase(info.tags, "Swapchain");
    info.is_eye = contains_icase(info.tags, "Eye");
    info.is_velocity_candidate = contains_icase(info.tags, "Velocity?");
    info.is_rt_pool = contains_icase(info.tags, "RT Pool");

    if (info.is_depth) {
        append_token(info.tags, "Depth", ", ");
    }

    if (info.is_render_target) {
        append_token(info.tags, "RT", ", ");
    }

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        append_token(info.tags, "UAV", ", ");
    }

    append_token(info.tags, extra_tags, ", ");
    info.is_transient = contains_icase(info.tags, "Transient");
    info.key = make_key(info.backend, info.pointer);
    m_live_d3d12_resources[info.key] = resource;

    merge_or_add(std::move(info));
}

std::optional<FrameResourceInspector::PreviewInfo> FrameResourceInspector::create_d3d11_preview(Framework& framework, uint64_t key) {
    auto live_it = m_live_d3d11_resources.find(key);
    if (live_it == m_live_d3d11_resources.end()) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D11;
        info.status = "Resource is not live this frame";
        return info;
    }

    auto* device = framework.get_d3d11_hook() != nullptr ? framework.get_d3d11_hook()->get_device() : nullptr;
    if (device == nullptr) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D11;
        info.status = "D3D11 device unavailable";
        return info;
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture{};
    if (FAILED(live_it->second.As(&texture))) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D11;
        info.status = "Selected resource is not a Texture2D";
        return info;
    }

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    const auto preview_format = preview_format_for_texture(desc.Format);
    if (!preview_format.has_value()) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D11;
        info.status = "No preview format available for this texture";
        return info;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = *preview_format;
    srv_desc.ViewDimension = desc.ArraySize > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DARRAY : D3D11_SRV_DIMENSION_TEXTURE2D;

    if (desc.ArraySize > 1) {
        srv_desc.Texture2DArray.MostDetailedMip = 0;
        srv_desc.Texture2DArray.MipLevels = desc.MipLevels == 0 ? UINT(-1) : desc.MipLevels;
        srv_desc.Texture2DArray.FirstArraySlice = 0;
        srv_desc.Texture2DArray.ArraySize = 1;
    } else {
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = desc.MipLevels == 0 ? UINT(-1) : desc.MipLevels;
    }

    m_d3d11_preview_srv.Reset();
    HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, &m_d3d11_preview_srv);

    if (FAILED(hr) && srv_desc.Format == desc.Format) {
        hr = device->CreateShaderResourceView(texture.Get(), nullptr, &m_d3d11_preview_srv);
    }

    PreviewInfo info{};
    info.has_selection = true;
    info.resource_key = key;
    info.backend = Backend::D3D11;
    info.width = desc.Width;
    info.height = desc.Height;
    info.format = dxgi_format_to_string(*preview_format);

    if (FAILED(hr) || m_d3d11_preview_srv == nullptr) {
        info.status = "Preview unavailable: SRV creation failed";
        return info;
    }

    info.available = true;
    info.texture_id = reinterpret_cast<uint64_t>(m_d3d11_preview_srv.Get());
    info.status = "Direct SRV preview";
    return info;
}

std::optional<FrameResourceInspector::PreviewInfo> FrameResourceInspector::create_d3d12_preview(Framework& framework, uint64_t key) {
    auto live_it = m_live_d3d12_resources.find(key);
    if (live_it == m_live_d3d12_resources.end()) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D12;
        info.status = "Resource is not live this frame";
        return info;
    }

    auto* device = framework.get_d3d12_hook() != nullptr ? framework.get_d3d12_hook()->get_device() : nullptr;
    if (device == nullptr) {
        PreviewInfo info{};
        info.has_selection = true;
        info.resource_key = key;
        info.backend = Backend::D3D12;
        info.status = "D3D12 device unavailable";
        return info;
    }

    auto* resource = live_it->second.Get();
    const auto desc = resource->GetDesc();

    PreviewInfo info{};
    info.has_selection = true;
    info.resource_key = key;
    info.backend = Backend::D3D12;
    info.width = static_cast<uint32_t>(desc.Width);
    info.height = desc.Height;

    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        info.status = "Preview unavailable: resource denies shader-resource views";
        return info;
    }

    const auto preview_format = preview_format_for_texture(desc.Format);
    if (!preview_format.has_value()) {
        info.status = "No preview format available for this texture";
        return info;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = *preview_format;
    srv_desc.ViewDimension = desc.DepthOrArraySize > 1 ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;

    if (desc.DepthOrArraySize > 1) {
        srv_desc.Texture2DArray.MostDetailedMip = 0;
        srv_desc.Texture2DArray.MipLevels = desc.MipLevels;
        srv_desc.Texture2DArray.FirstArraySlice = 0;
        srv_desc.Texture2DArray.ArraySize = 1;
        srv_desc.Texture2DArray.PlaneSlice = 0;
        srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    } else {
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = desc.MipLevels;
        srv_desc.Texture2D.PlaneSlice = 0;
        srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    }

    device->CreateShaderResourceView(resource, &srv_desc, framework.m_d3d12.get_cpu_srv(device, Framework::D3D12::SRV::INSPECTOR_PREVIEW));

    info.available = true;
    info.format = dxgi_format_to_string(*preview_format);
    info.texture_id = framework.m_d3d12.get_gpu_srv(device, Framework::D3D12::SRV::INSPECTOR_PREVIEW).ptr;
    info.status = "Direct descriptor preview";
    info.backend_note = "Preview uses a direct SRV alias; some resources may still be unavailable or imperfect if the source resource is not preview-safe.";
    return info;
}

void FrameResourceInspector::sample_d3d11(Framework& framework, VR& vr) {
    track_d3d11_resource(framework.m_d3d11.bb_tex.Get(), "Framework Backbuffer", "Framework/Swapchain", "Swapchain");
    track_d3d11_resource(framework.m_d3d11.rt.Get(), "Framework ImGui Render Target", "Framework/UI", "UI");
    track_d3d11_resource(framework.m_d3d11.blank_rt.Get(), "Framework Blank Render Target", "Framework/UI", "UI");

    if (framework.get_d3d11_hook() != nullptr) {
        track_d3d11_resource(framework.get_d3d11_hook()->get_last_depthstencil_used().Get(), "Last Bound Depth", "Hook/Depth", "Depth");
    }

    auto& d3d11 = vr.d3d11();
    track_d3d11_resource(d3d11.m_ui_tex.Get(), "VR UI Texture", "VR/UI", "UI");
    track_d3d11_resource(d3d11.m_engine_ui_ref.tex.Get(), "VR Engine UI Ref", "VR/UI", "UI");
    track_d3d11_resource(d3d11.m_engine_tex_ref.tex.Get(), "VR Game Texture", "VR/Scene");
    track_d3d11_resource(d3d11.m_scene_capture_tex_ref.tex.Get(), "VR Scene Capture Ref", "VR/Scene");
    track_d3d11_resource(d3d11.m_2d_screen_tex[0].tex.Get(), "VR 2D Screen [0]", "VR/UI", "UI");
    track_d3d11_resource(d3d11.m_2d_screen_tex[1].tex.Get(), "VR 2D Screen [1]", "VR/UI", "UI");
    track_d3d11_resource(d3d11.m_left_eye_tex.Get(), "VR Left Eye", "VR/Eye", "Eye");
    track_d3d11_resource(d3d11.m_right_eye_tex.Get(), "VR Right Eye", "VR/Eye", "Eye");
    track_d3d11_resource(d3d11.m_backbuffer.Get(), "VR Backbuffer Copy", "VR/Swapchain");
    track_d3d11_resource(d3d11.m_spectator_view_backbuffer.Get(), "VR Spectator Backbuffer", "VR/Swapchain");
    track_d3d11_resource(d3d11.m_extreme_compat_backbuffer.Get(), "VR Extreme Compat Backbuffer", "VR/Swapchain");
    track_d3d11_resource(d3d11.m_converted_backbuffer.Get(), "VR Converted Backbuffer", "VR/Swapchain");

    for (const auto& [swapchain_idx, ctx] : d3d11.m_openxr.contexts) {
        for (size_t tex_idx = 0; tex_idx < ctx.textures.size(); ++tex_idx) {
            const auto* texture = ctx.textures[tex_idx].texture;
            track_d3d11_resource(
                texture,
                "OpenXR Swapchain " + std::to_string(swapchain_idx) + " [" + std::to_string(tex_idx) + "]",
                "VR/OpenXR",
                "Eye"
            );
        }
    }

    if (auto& rt_pool = vr.get_render_target_pool_hook(); rt_pool != nullptr) {
        for (const auto& name : rt_pool->snapshot_render_target_names()) {
            const auto texture = rt_pool->get_texture<ID3D11Resource>(name);
            track_d3d11_resource(texture.Get(), utility::narrow(name), "RT Pool");
        }
    }
}

void FrameResourceInspector::sample_d3d12(Framework& framework, VR& vr) {
    auto swapchain = framework.get_d3d12_hook() != nullptr ? framework.get_d3d12_hook()->get_swap_chain() : nullptr;
    const auto current_backbuffer_index = swapchain != nullptr ? swapchain->GetCurrentBackBufferIndex() : 0U;

    for (int i = static_cast<int>(Framework::D3D12::RTV::BACKBUFFER_0); i <= static_cast<int>(Framework::D3D12::RTV::BACKBUFFER_8); ++i) {
        const auto index = static_cast<uint32_t>(i - static_cast<int>(Framework::D3D12::RTV::BACKBUFFER_0));
        auto* resource = framework.m_d3d12.rts[i].Get();

        if (resource == nullptr) {
            continue;
        }

        auto name = std::string{"Framework Backbuffer ["} + std::to_string(index) + "]";
        auto tags = std::string{"Swapchain"};

        if (index == current_backbuffer_index) {
            name += " Active";
            append_token(tags, "Current", ", ");
        }

        track_d3d12_resource(resource, name, "Framework/Swapchain", tags);
    }

    track_d3d12_resource(framework.m_d3d12.get_rt(Framework::D3D12::RTV::IMGUI).Get(), "Framework ImGui Render Target", "Framework/UI", "UI");
    track_d3d12_resource(framework.m_d3d12.get_rt(Framework::D3D12::RTV::BLANK).Get(), "Framework Blank Render Target", "Framework/UI", "UI");

    auto& d3d12 = vr.d3d12();
    track_d3d12_resource(d3d12.m_prev_backbuffer.Get(), "VR Previous Backbuffer", "VR/Swapchain");
    track_d3d12_resource(d3d12.m_backbuffer_copy.texture.Get(), "VR Backbuffer Copy", "VR/Swapchain");
    track_d3d12_resource(d3d12.m_game_ui_tex.texture.Get(), "VR Game UI", "VR/UI", "UI");
    track_d3d12_resource(d3d12.m_game_tex.texture.Get(), "VR Game Texture", "VR/Scene");
    track_d3d12_resource(d3d12.m_scene_capture_tex.texture.Get(), "VR Scene Capture", "VR/Scene");
    track_d3d12_resource(d3d12.m_2d_screen_tex[0].texture.Get(), "VR 2D Screen [0]", "VR/UI", "UI");
    track_d3d12_resource(d3d12.m_2d_screen_tex[1].texture.Get(), "VR 2D Screen [1]", "VR/UI", "UI");

    for (size_t i = 0; i < d3d12.m_backbuffer_textures.size(); ++i) {
        const auto& texture = d3d12.m_backbuffer_textures[i];
        if (texture != nullptr) {
            track_d3d12_resource(texture->texture.Get(), "VR Backbuffer Texture [" + std::to_string(i) + "]", "VR/Swapchain");
        }
    }

    for (size_t i = 0; i < d3d12.m_openvr.left_eye_tex.size(); ++i) {
        track_d3d12_resource(d3d12.m_openvr.left_eye_tex[i].texture.Get(), "OpenVR Left Eye [" + std::to_string(i) + "]", "VR/OpenVR", "Eye");
        track_d3d12_resource(d3d12.m_openvr.right_eye_tex[i].texture.Get(), "OpenVR Right Eye [" + std::to_string(i) + "]", "VR/OpenVR", "Eye");
    }

    track_d3d12_resource(d3d12.m_openvr.ui_tex.texture.Get(), "OpenVR UI Texture", "VR/OpenVR", "UI");

    for (const auto& [swapchain_idx, ctx] : d3d12.m_openxr.contexts) {
        for (size_t tex_idx = 0; tex_idx < ctx.texture_contexts.size(); ++tex_idx) {
            const auto& texture_context = ctx.texture_contexts[tex_idx];
            if (texture_context != nullptr) {
                track_d3d12_resource(
                    texture_context->texture.Get(),
                    "OpenXR Swapchain " + std::to_string(swapchain_idx) + " [" + std::to_string(tex_idx) + "]",
                    "VR/OpenXR",
                    "Eye"
                );
            }
        }
    }

    if (auto& rt_pool = vr.get_render_target_pool_hook(); rt_pool != nullptr) {
        for (const auto& name : rt_pool->snapshot_render_target_names()) {
            const auto texture = rt_pool->get_texture<ID3D12Resource>(name);
            track_d3d12_resource(texture.Get(), utility::narrow(name), "RT Pool");
        }
    }
}
} // namespace render
