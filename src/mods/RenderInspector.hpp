#pragma once

#include <optional>
#include <string>

#include "Mod.hpp"
#include "render/D3D12Diagnostics.hpp"
#include "render/FrameResourceInspector.hpp"
#include "render/RenderAnalysisExport.hpp"
#include "render/ShaderOverrideRegistry.hpp"

class RenderInspector : public Mod {
public:
    static std::shared_ptr<RenderInspector>& get();

    std::string_view get_name() const override {
        return "Render Inspector";
    }

    std::vector<SidebarEntryInfo> get_sidebar_entries() override {
        return {
            {"Resources", false},
            {"DX12 Diagnostics", false},
            {"PSO Profiler", false},
            {"Shaders", false},
        };
    }

    void on_present() override;
    void on_draw_sidebar_entry(std::string_view in_entry) override;

private:
    void draw_resources();
    void draw_dx12_diagnostics();
    void draw_pso_profiler();
    void draw_shaders();

    render::FrameResourceInspector m_inspector{};
    std::optional<uint64_t> m_selected_resource_key{};
    bool m_filter_depth_only{false};
    bool m_filter_render_targets_only{false};
    bool m_filter_ui_only{false};
    bool m_filter_swapchain_only{false};
    bool m_filter_recent_only{true};
    int m_recent_frame_window{180};
    int m_dx12_event_limit{24};
    int m_recent_dx12_shader_pair_limit{16};
    bool m_freeze_dx12_live_view{false};
    int m_dx12_live_sample_interval_frames{15};
    uint64_t m_last_dx12_live_sample_frame{};
    std::optional<render::ShaderOverrideRegistry::D3D12PipelinePairInfo> m_displayed_dx12_pair{};
    bool m_sort_recent_dx12_pairs_by_hits{true};
    std::string m_selected_recent_dx12_pair_key{};
    std::string m_shader_export_status{};
    bool m_pso_filter_overridden_only{false};
    bool m_pso_filter_stream_only{false};
    bool m_pso_filter_with_targets_only{false};
    bool m_pso_filter_tracking_warnings_only{false};
    int m_pso_profiler_limit{32};
    int m_pso_sort_mode{0};
    std::string m_selected_pso_key{};
    std::string m_render_bundle_export_status{};
};
