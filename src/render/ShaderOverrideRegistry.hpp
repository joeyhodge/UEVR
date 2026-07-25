#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "render/ShaderCompiler.hpp"

class Framework;

namespace render {
class ShaderOverrideRegistry {
public:
    enum class Backend : uint8_t {
        D3D11,
        D3D12,
    };

    enum class Stage : uint8_t {
        Vertex,
        Pixel,
    };

    using CreateVertexShaderFn = HRESULT (WINAPI*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
    using CreatePixelShaderFn = HRESULT (WINAPI*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);

    struct BoundShaderInfo {
        bool known{};
        Backend backend{Backend::D3D11};
        Stage stage{Stage::Vertex};
        uintptr_t original_pointer{};
        uintptr_t bound_pointer{};
        std::string hash{};
        bool override_active{};
        std::string override_name{};
        std::string note{};
        uint64_t last_bound_frame{};
    };

    struct OverrideEntryInfo {
        std::string key{};
        std::string name{};
        Backend backend{Backend::D3D11};
        Stage stage{Stage::Vertex};
        std::string target_hash{};
        std::string manifest_path{};
        std::string source_path{};
        std::string entry_point{};
        std::string profile{};
        bool enabled{};
        bool compiled{};
        bool apply_supported{};
        bool from_profile_dir{};
        uint64_t generation{};
        std::string status{};
        std::string compiler{};
        std::string last_error{};
    };

    struct D3D12PipelinePairInfo {
        uint64_t frame{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uint64_t hit_count{};
        uintptr_t original_pipeline_state{};
        uintptr_t bound_pipeline_state{};
        bool pipeline_stream{};
        std::string tracking_note{};
        BoundShaderInfo vertex_shader{};
        BoundShaderInfo pixel_shader{};
    };

    struct PsoRenderUsageInfo {
        std::string render_target_name{};
        std::string depth_target_name{};
        std::string render_target_key{};
        std::string depth_target_key{};
        uint64_t hit_count{};
        double share{};
    };

    struct D3D12PsoAggregateInfo {
        uint64_t total_samples{};
        double sample_share{};
        uint64_t bind_count_with_known_targets{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uintptr_t original_pso{};
        uintptr_t last_bound_pso{};
        bool pipeline_stream{};
        std::string tracking_note{};
        std::string vs_hash{};
        std::string ps_hash{};
        std::string vs_override{};
        std::string ps_override{};
        std::vector<PsoRenderUsageInfo> likely_targets{};
    };

    struct Snapshot {
        bool auto_reload{true};
        uint64_t frame{};
        std::string global_override_dir{};
        std::string profile_override_dir{};
        BoundShaderInfo bound_vertex_shader{};
        BoundShaderInfo bound_pixel_shader{};
        std::optional<D3D12PipelinePairInfo> current_d3d12_pair{};
        bool capture_next_d3d12_change_armed{};
        std::optional<D3D12PipelinePairInfo> captured_d3d12_pair{};
        uint64_t total_d3d12_pair_samples{};
        std::vector<D3D12PipelinePairInfo> distinct_d3d12_pairs{};
        uint64_t total_d3d12_pso_samples{};
        std::vector<D3D12PsoAggregateInfo> d3d12_pso_aggregates{};
        std::vector<OverrideEntryInfo> overrides{};
        std::vector<std::string> recent_events{};
    };

    static ShaderOverrideRegistry& get();

    void on_present(Framework& framework);
    void set_inspector_tracking_enabled(bool enabled);
    bool should_track_d3d11_shaders() const;
    bool should_track_d3d12_pipelines() const;
    bool should_collect_d3d12_inspector_data() const;
    void request_reload();
    void request_capture_next_d3d12_change();
    void clear_captured_d3d12_change();
    bool export_d3d12_pairs_json(std::filesystem::path& out_path, std::string& error_out);
    bool export_d3d12_pairs_csv(std::filesystem::path& out_path, std::string& error_out);
    Snapshot snapshot(bool include_live_d3d12_tracking = true) const;

    void set_d3d11_create_callbacks(CreateVertexShaderFn create_vs, CreatePixelShaderFn create_ps);
    void register_d3d11_shader_creation(Stage stage, ID3D11Device* device, IUnknown* shader, const void* bytecode, size_t bytecode_size);
    ID3D11VertexShader* resolve_d3d11_vertex_shader(ID3D11Device* device, ID3D11VertexShader* shader);
    ID3D11PixelShader* resolve_d3d11_pixel_shader(ID3D11Device* device, ID3D11PixelShader* shader);
    void note_d3d11_shader_bound(Stage stage, IUnknown* original_shader, IUnknown* bound_shader);
    void register_d3d12_graphics_pipeline_state_creation(ID3D12Device* device, ID3D12PipelineState* pipeline_state, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);
    void register_d3d12_pipeline_state_stream_creation(ID3D12Device* device, ID3D12PipelineState* pipeline_state, const D3D12_PIPELINE_STATE_STREAM_DESC* desc);
    ID3D12PipelineState* resolve_d3d12_pipeline_state(ID3D12PipelineState* pipeline_state);
    void note_d3d12_pipeline_state_bound(ID3D12PipelineState* original_pipeline_state, ID3D12PipelineState* bound_pipeline_state);

private:
    struct OverrideEntry {
        std::string key{};
        std::string name{};
        Backend backend{Backend::D3D11};
        Stage stage{Stage::Vertex};
        std::string target_hash{};
        std::filesystem::path manifest_path{};
        std::filesystem::path source_path{};
        std::string entry_point{};
        std::string profile{};
        ShaderCompilerBackend preferred_compiler{ShaderCompilerBackend::Auto};
        bool enabled{true};
        bool from_profile_dir{};
        bool compiled{};
        bool apply_supported{};
        uint64_t generation{};
        std::string status{};
        std::string compiler{};
        std::string last_error{};
        std::vector<uint8_t> compiled_bytecode{};
        std::filesystem::file_time_type manifest_write_time{};
        std::filesystem::file_time_type source_write_time{};
    };

    struct D3D11ShaderRecord {
        Stage stage{Stage::Vertex};
        uintptr_t shader_pointer{};
        uintptr_t device_pointer{};
        std::string hash{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uint64_t seen_count{};
        uint64_t override_generation{};
        bool override_active{};
        std::string override_name{};
        Microsoft::WRL::ComPtr<ID3D11DeviceChild> override_shader{};
    };

    struct OwnedD3D12GraphicsPipelineStateDesc {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature{};
        std::vector<uint8_t> vertex_shader{};
        std::vector<uint8_t> pixel_shader{};
        std::vector<uint8_t> domain_shader{};
        std::vector<uint8_t> hull_shader{};
        std::vector<uint8_t> geometry_shader{};
        std::vector<std::string> input_semantic_names{};
        std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements{};
        std::vector<std::string> stream_output_semantic_names{};
        std::vector<D3D12_SO_DECLARATION_ENTRY> stream_output_declarations{};
        std::vector<UINT> stream_output_strides{};

        void refresh_views();
    };

    struct OwnedD3D12PipelineStateStream {
        D3D12_PIPELINE_STATE_STREAM_DESC desc{};
        std::vector<uint8_t> stream_bytes{};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature{};
        std::vector<uint8_t> vertex_shader{};
        std::vector<uint8_t> pixel_shader{};
        std::vector<uint8_t> domain_shader{};
        std::vector<uint8_t> hull_shader{};
        std::vector<uint8_t> geometry_shader{};
        std::vector<uint8_t> compute_shader{};
        std::vector<uint8_t> amplification_shader{};
        std::vector<uint8_t> mesh_shader{};
        std::vector<std::string> input_semantic_names{};
        std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements{};
        std::vector<std::string> stream_output_semantic_names{};
        std::vector<D3D12_SO_DECLARATION_ENTRY> stream_output_declarations{};
        std::vector<UINT> stream_output_strides{};
        std::vector<D3D12_VIEW_INSTANCE_LOCATION> view_instance_locations{};
        size_t root_signature_offset{static_cast<size_t>(-1)};
        size_t vertex_shader_offset{static_cast<size_t>(-1)};
        size_t pixel_shader_offset{static_cast<size_t>(-1)};
        size_t domain_shader_offset{static_cast<size_t>(-1)};
        size_t hull_shader_offset{static_cast<size_t>(-1)};
        size_t geometry_shader_offset{static_cast<size_t>(-1)};
        size_t compute_shader_offset{static_cast<size_t>(-1)};
        size_t amplification_shader_offset{static_cast<size_t>(-1)};
        size_t mesh_shader_offset{static_cast<size_t>(-1)};
        size_t input_layout_offset{static_cast<size_t>(-1)};
        size_t stream_output_offset{static_cast<size_t>(-1)};
        size_t cached_pso_offset{static_cast<size_t>(-1)};
        size_t view_instancing_offset{static_cast<size_t>(-1)};

        void refresh_views();
        bool empty() const {
            return stream_bytes.empty();
        }
    };

    struct D3D12GraphicsPsoRecord {
        uintptr_t pipeline_state_pointer{};
        std::string vertex_hash{};
        std::string pixel_hash{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uint64_t seen_count{};
        uint64_t applied_override_revision{};
        bool is_pipeline_stream{};
        bool override_active{};
        std::string vertex_override_name{};
        std::string pixel_override_name{};
        std::string tracking_note{};
        std::string last_error{};
        Microsoft::WRL::ComPtr<ID3D12Device> device{};
        Microsoft::WRL::ComPtr<ID3D12PipelineState> override_pipeline_state{};
        OwnedD3D12GraphicsPipelineStateDesc owned_desc{};
        OwnedD3D12PipelineStateStream owned_stream{};
    };

    struct PsoRenderUsageRecord {
        std::string render_target_name{};
        std::string depth_target_name{};
        std::string render_target_key{};
        std::string depth_target_key{};
        uint64_t hit_count{};
    };

    struct D3D12PsoAggregateRecord {
        uintptr_t original_pso{};
        uintptr_t last_bound_pso{};
        bool pipeline_stream{};
        std::string tracking_note{};
        std::string vs_hash{};
        std::string ps_hash{};
        std::string vs_override{};
        std::string ps_override{};
        uint64_t total_samples{};
        uint64_t bind_count_with_known_targets{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        std::unordered_map<std::string, PsoRenderUsageRecord> usage_by_key{};
    };

    void scan_override_directories();
    void scan_single_directory(
        const std::filesystem::path& dir,
        bool from_profile_dir,
        std::unordered_map<std::string, OverrideEntry>& discovered_entries);
    void remove_deleted_entries(const std::unordered_map<std::string, std::filesystem::path>& discovered_entries);
    void compile_or_refresh_entry(OverrideEntry& entry);
    std::optional<OverrideEntry> parse_manifest(const std::filesystem::path& manifest_path, bool from_profile_dir);
    bool compile_entry(OverrideEntry& entry, std::string& error_out);
    void push_event(std::string message);
    void refresh_active_override_flags_locked();
    void update_d3d11_override_shader(D3D11ShaderRecord& record, ID3D11Device* device);
    void update_d3d12_override_pipeline_state(D3D12GraphicsPsoRecord& record);
    static bool copy_pipeline_state_stream(const D3D12_PIPELINE_STATE_STREAM_DESC* desc, OwnedD3D12PipelineStateStream& out, std::string& error_out);
    void record_d3d12_pipeline_pair(const D3D12PipelinePairInfo& info);
    void record_d3d12_pso_sample(const D3D12PipelinePairInfo& info);
    std::string make_d3d12_pair_key(const D3D12PipelinePairInfo& info) const;
    std::string make_d3d12_pso_key(const D3D12PipelinePairInfo& info) const;
    std::filesystem::path make_d3d12_pair_export_path(const char* extension) const;
    std::string make_override_key(Backend backend, Stage stage, std::string_view target_hash) const;
    std::string hash_shader_bytecode(const void* bytecode, size_t bytecode_size) const;
    std::filesystem::path global_override_dir() const;
    std::filesystem::path profile_override_dir() const;

    mutable std::recursive_mutex m_mutex{};
    std::unordered_map<std::string, OverrideEntry> m_overrides{};
    std::unordered_map<uintptr_t, D3D11ShaderRecord> m_d3d11_shader_records{};
    std::unordered_map<uintptr_t, D3D12GraphicsPsoRecord> m_d3d12_graphics_pso_records{};
    BoundShaderInfo m_bound_vertex_shader{};
    BoundShaderInfo m_bound_pixel_shader{};
    bool m_capture_next_d3d12_change{};
    std::optional<D3D12PipelinePairInfo> m_captured_d3d12_pair{};
    std::optional<D3D12PipelinePairInfo> m_last_d3d12_pair{};
    uint64_t m_total_d3d12_pair_samples{};
    std::vector<D3D12PipelinePairInfo> m_distinct_d3d12_pairs{};
    std::unordered_map<std::string, size_t> m_distinct_d3d12_pair_indices{};
    uint64_t m_total_d3d12_pso_samples{};
    std::unordered_map<std::string, D3D12PsoAggregateRecord> m_d3d12_pso_aggregates{};
    std::vector<std::string> m_recent_events{};
    std::atomic<int64_t> m_last_scan_time_ns{};
    std::atomic_bool m_force_reload{};
    std::atomic<uint64_t> m_frame{};
    uint64_t m_override_revision{};
    CreateVertexShaderFn m_create_vertex_shader{};
    CreatePixelShaderFn m_create_pixel_shader{};
    std::atomic_bool m_has_active_d3d11_overrides{false};
    std::atomic_bool m_has_active_d3d12_overrides{false};
    std::atomic_bool m_inspector_tracking_enabled{false};
    std::atomic_bool m_capture_next_d3d12_change_hot_path{false};
};
} // namespace render
