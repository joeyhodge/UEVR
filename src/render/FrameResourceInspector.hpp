#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

class Framework;
class VR;

namespace render {
class FrameResourceInspector {
public:
    enum class Backend : uint8_t {
        D3D11,
        D3D12,
    };

    struct ResourceInfo {
        uint64_t key{};
        Backend backend{Backend::D3D11};
        uintptr_t pointer{};
        std::string name{};
        std::string source{};
        std::string type{"Texture2D"};
        std::string format{};
        std::string resolution{};
        std::string tags{};
        uint32_t width{};
        uint32_t height{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uint64_t seen_count{};
        uint64_t change_count{};
        bool is_depth{false};
        bool is_render_target{false};
        bool is_ui{false};
        bool is_swapchain{false};
        bool is_eye{false};
        bool is_velocity_candidate{false};
        bool is_rt_pool{false};
        bool is_transient{false};
        bool is_recent{false};
    };

    struct PreviewInfo {
        bool has_selection{false};
        bool available{false};
        Backend backend{Backend::D3D11};
        uint64_t resource_key{};
        uint64_t texture_id{};
        uint32_t width{};
        uint32_t height{};
        std::string format{};
        std::string status{};
        std::string backend_note{};
    };

    void on_present(Framework& framework, VR& vr);
    std::vector<ResourceInfo> snapshot() const;
    uint64_t current_frame() const;
    void reset();
    void set_selected_resource(std::optional<uint64_t> key);
    std::optional<uint64_t> selected_resource() const;
    PreviewInfo preview_info() const;

private:
    uint64_t make_key(Backend backend, uintptr_t pointer) const;
    void prune_stale_resources();
    void refresh_selected_preview(Framework& framework);

    void sample_d3d11(Framework& framework, VR& vr);
    void sample_d3d12(Framework& framework, VR& vr);

    void track_d3d11_resource(
        const ID3D11Resource* resource,
        const std::string& name,
        const std::string& source,
        const std::string& extra_tags = {}
    );

    void track_d3d12_resource(
        ID3D12Resource* resource,
        const std::string& name,
        const std::string& source,
        const std::string& extra_tags = {}
    );

    void merge_or_add(ResourceInfo incoming);
    std::optional<PreviewInfo> create_d3d11_preview(Framework& framework, uint64_t key);
    std::optional<PreviewInfo> create_d3d12_preview(Framework& framework, uint64_t key);

    mutable std::recursive_mutex m_mutex{};
    std::unordered_map<uint64_t, ResourceInfo> m_resources{};
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D11Resource>> m_live_d3d11_resources{};
    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>> m_live_d3d12_resources{};
    std::optional<uint64_t> m_selected_key{};
    PreviewInfo m_preview{};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_d3d11_preview_srv{};
    uint64_t m_frame_index{};
};
} // namespace render
