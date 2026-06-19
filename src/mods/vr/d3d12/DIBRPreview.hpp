#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl.h>

#include "TextureContext.hpp"
#include <hooks/D3D12Hook.hpp>

namespace d3d12 {
// A deliberately small, fail-closed DIBR path. It is kept separate from the
// experimental AFW and native-array paths so selecting DIBR cannot alter them.
class DIBRPreview : public D3D12DepthStencilObserver {
public:
    DIBRPreview() = default;

    struct Parameters {
        float disparity_pixels{18.0f};
        bool reversed_depth{true};
        // When the runtime projections and eye baseline are valid, this maps
        // left-eye clip space directly into right-eye clip space. The legacy
        // screen-space shift remains the fail-closed fallback.
        bool use_true_reprojection{false};
        std::array<float, 16> source_to_right{};

        // This remains independent from the legacy disparity slider so a title
        // can tune the real projection solve without changing fallback behavior.
        float reprojection_strength{1.0f};
        float legacy_depth_curve{1.0f};
        float legacy_near_depth_cap{1.0f};
        bool depth_edge_stabilization{};
        float depth_edge_threshold{0.01f};
        float depth_edge_stabilization_strength{0.75f};
    };

    ~DIBRPreview();

    DIBRPreview(const DIBRPreview&) = delete;
    DIBRPreview& operator=(const DIBRPreview&) = delete;

    bool synthesize(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* color,
        D3D12_RESOURCE_STATES color_state,
        ID3D12Resource* depth,
        D3D12_RESOURCE_STATES depth_state,
        Parameters parameters);

    void reset();

    bool ready() const { return m_status.load(std::memory_order_acquire) == Status::Ready; }
    bool failed() const { return m_status.load(std::memory_order_acquire) == Status::Failed; }
    const char* status_name() const;
    std::string failure_reason() const;
    std::string depth_trace_summary() const;
    const TextureContext& output() const { return m_output; }
    Microsoft::WRL::ComPtr<ID3D12Resource> captured_depth_snapshot() const;
    bool has_captured_depth() const;

    void set_depth_trace_expected_extent(uint32_t width, uint32_t height);
    void set_ue5_rdg_depth_capture_enabled(bool enabled);

    void on_depth_stencil_view_created(
        ID3D12Resource* resource,
        const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override;
    void on_resource_barriers(
        ID3D12GraphicsCommandList* command_list,
        UINT count,
        const D3D12_RESOURCE_BARRIER* barriers) override;

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    enum class Status : uint8_t {
        Idle,
        Waiting,
        Ready,
        Failed,
    };

    struct alignas(256) Constants {
        uint32_t output_width{};
        uint32_t output_height{};
        uint32_t source_eye_width{};
        uint32_t source_height{};
        uint32_t depth_width{};
        uint32_t depth_height{};
        uint32_t depth_is_double_wide{};
        uint32_t reversed_depth{};
        float disparity_pixels{};
        float edge_feather{0.08f};
        uint32_t use_true_reprojection{};
        float true_reprojection_strength{};
        float source_to_right[16]{};
        float legacy_depth_curve{1.0f};
        float legacy_near_depth_cap{1.0f};
        uint32_t enable_depth_edge_stabilization{};
        float depth_edge_threshold{0.01f};
        float depth_edge_stabilization_strength{0.75f};
        float quality_padding[2]{};
        float padding[28]{};
    };
    static_assert(sizeof(Constants) == 256, "DIBR constants must remain one CBV-aligned block");

    bool ensure_pipeline(ID3D12Device* device);
    bool ensure_output(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format);
    bool ensure_source_staging(ID3D12Device* device, const D3D12_RESOURCE_DESC& color_desc);
    void stage_left_eye(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* color,
        D3D12_RESOURCE_STATES color_state);
    bool validate_inputs(ID3D12Resource* color, ID3D12Resource* depth, std::string& reason) const;
    bool create_descriptors(ID3D12Device* device, ID3D12Resource* depth);
    bool capture_depth_before_restore_locked(
        ID3D12GraphicsCommandList* command_list,
        ID3D12Resource* source,
        D3D12_RESOURCE_STATES source_state,
        uint32_t source_array_slice);
    bool ensure_captured_depth_locked(ID3D12Device* device, const D3D12_RESOURCE_DESC& source_desc, std::string& reason);
    bool is_trace_candidate_compatible_locked(uintptr_t resource) const;
    void fail(std::string reason);

    static DXGI_FORMAT color_srv_format(DXGI_FORMAT format);
    static DXGI_FORMAT depth_srv_format(DXGI_FORMAT format);
    void refresh_depth_trace_candidate_locked();

    struct DepthTraceCandidate {
        uintptr_t resource{};
        uint64_t width{};
        uint32_t height{};
        DXGI_FORMAT resource_format{DXGI_FORMAT_UNKNOWN};
        D3D12_RESOURCE_FLAGS flags{D3D12_RESOURCE_FLAG_NONE};
        uint32_t sample_count{};
        DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
        uint16_t array_size{};
        uint32_t array_slice{};
    };

    std::atomic<Status> m_status{Status::Idle};
    mutable std::mutex m_status_mutex{};
    std::string m_failure_reason{};
    std::string m_capture_failure_reason{};
    std::unordered_map<uintptr_t, DepthTraceCandidate> m_traced_depth_resources{};
    std::string m_depth_trace_summary{"waiting for a DSV candidate"};
    uint64_t m_depth_trace_expected_width{};
    uint32_t m_depth_trace_expected_height{};
    std::atomic<uintptr_t> m_depth_trace_candidate{};
    std::atomic<uint32_t> m_depth_trace_candidate_array_slice{};
    std::atomic<uint32_t> m_depth_trace_last_state{D3D12_RESOURCE_STATE_COMMON};
    std::atomic<bool> m_ue5_rdg_depth_capture_enabled{false};
    mutable std::mutex m_capture_mutex{};
    ComPtr m_captured_depth{};
    uint64_t m_captured_depth_width{};
    uint32_t m_captured_depth_height{};
    DXGI_FORMAT m_captured_depth_format{DXGI_FORMAT_UNKNOWN};
    uint64_t m_captured_depth_generation{};
    bool m_capture_success_logged{};
    bool m_capture_failure_logged{};
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_root_signature{};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipeline_state{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_descriptor_heap{};
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constants{};
    uint8_t* m_constants_cpu{};
    // Keep the game's typeless packed backbuffer out of the compute descriptor
    // table. DIBR samples this owned, single-eye staging copy instead.
    ComPtr m_source_staging{};
    uint32_t m_source_staging_width{};
    uint32_t m_source_staging_height{};
    DXGI_FORMAT m_source_staging_format{DXGI_FORMAT_UNKNOWN};
    TextureContext m_output{};
    uint32_t m_output_width{};
    uint32_t m_output_height{};
    DXGI_FORMAT m_output_format{DXGI_FORMAT_UNKNOWN};
    uint32_t m_descriptor_increment{};
    bool m_output_has_been_written{};
};
} // namespace d3d12
