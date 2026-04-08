#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <dxgi1_4.h>

namespace render {
class D3D12Diagnostics {
public:
    struct HeapInfo {
        uintptr_t pointer{};
        std::string name{};
        std::string source{};
        std::string type{};
        uint32_t total_descriptors{};
        uint32_t estimated_in_use{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        uint64_t bind_count{};
        bool shader_visible{};
        bool transient{};
        bool is_active{};
    };

    struct BarrierEvent {
        uint64_t frame{};
        std::string source{};
        uintptr_t resource{};
        std::string type{};
        std::string before_state{};
        std::string after_state{};
        uint32_t subresource{};
        std::string note{};
    };

    struct BindingEvent {
        uint64_t frame{};
        std::string source{};
        std::string kind{};
        std::string detail{};
    };

    struct WarningEvent {
        uint64_t frame{};
        std::string source{};
        std::string message{};
    };

    struct BoundTargetInfo {
        uintptr_t handle{};
        uintptr_t resource{};
        std::string name{};
        std::string descriptor_type{};
    };

    struct CurrentBindContext {
        uint64_t frame{};
        std::string source{};
        std::vector<BoundTargetInfo> render_targets{};
        std::optional<BoundTargetInfo> depth_target{};
        bool exact_this_frame{};
    };

    struct Snapshot {
        bool available{};
        uint64_t frame{};
        uintptr_t device{};
        uintptr_t swapchain{};
        uintptr_t command_queue{};
        uint32_t render_width{};
        uint32_t render_height{};
        uint32_t display_width{};
        uint32_t display_height{};
        bool proton_swapchain{};
        bool framegen_swapchain{};
        uintptr_t active_cbv_srv_uav_heap{};
        uintptr_t active_sampler_heap{};
        uint32_t descriptor_heap_sets_this_frame{};
        uint32_t descriptor_heap_switches_this_frame{};
        uint32_t resource_barriers_this_frame{};
        uint32_t rtv_binds_this_frame{};
        uint32_t transient_heap_creations_this_frame{};
        uint32_t transient_resource_creations_this_frame{};
        uint64_t transient_resource_bytes_this_frame{};
        uint64_t tracked_resource_bytes_total{};
        uint64_t tracked_transient_resource_bytes_total{};
        std::optional<CurrentBindContext> current_bind_context{};
        std::vector<HeapInfo> heaps{};
        std::vector<BindingEvent> recent_bindings{};
        std::vector<BarrierEvent> recent_barriers{};
        std::vector<WarningEvent> recent_warnings{};
    };

    static D3D12Diagnostics& get();

    void set_enabled(bool enabled);
    bool is_enabled() const;

    void begin_frame(
        ID3D12Device* device,
        IDXGISwapChain3* swapchain,
        ID3D12CommandQueue* queue,
        uint32_t render_width,
        uint32_t render_height,
        uint32_t display_width,
        uint32_t display_height,
        bool proton_swapchain,
        bool framegen_swapchain
    );

    void register_descriptor_heap(
        std::string_view source,
        ID3D12DescriptorHeap* heap,
        uint32_t estimated_in_use = 0,
        bool transient = false,
        std::string_view name = {}
    );

    void register_resource(
        std::string_view source,
        ID3D12Resource* resource,
        bool transient = false,
        std::string_view name = {}
    );

    void register_rtv_descriptor(
        std::string_view source,
        ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        std::string_view name = {}
    );

    void register_dsv_descriptor(
        std::string_view source,
        ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE handle,
        std::string_view name = {}
    );

    void record_descriptor_heaps_set(
        std::string_view source,
        uint32_t count,
        ID3D12DescriptorHeap* const* heaps
    );

    void record_resource_barriers(
        std::string_view source,
        uint32_t count,
        const D3D12_RESOURCE_BARRIER* barriers
    );

    void record_rtv_bind(
        std::string_view source,
        uint32_t rtv_count,
        const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsv
    );

    Snapshot snapshot() const;
    std::optional<CurrentBindContext> current_bind_context() const;
    void reset();

private:
    struct ResourceInfo {
        uintptr_t pointer{};
        std::string name{};
        std::string source{};
        std::string format{};
        uint32_t width{};
        uint32_t height{};
        uint64_t approx_bytes{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
        bool transient{};
    };

    struct DescriptorInfo {
        uintptr_t handle{};
        uintptr_t resource{};
        std::string name{};
        std::string source{};
        std::string descriptor_type{};
        uint64_t first_seen_frame{};
        uint64_t last_seen_frame{};
    };

    void push_warning(std::string_view source, std::string message);
    void note_frame_warning_if_needed();
    void clear_state_locked();

    std::atomic_bool m_enabled{false};
    mutable std::recursive_mutex m_mutex{};
    std::unordered_map<uintptr_t, HeapInfo> m_heaps{};
    std::unordered_map<uintptr_t, ResourceInfo> m_resources{};
    std::unordered_map<uintptr_t, DescriptorInfo> m_rtv_descriptors{};
    std::unordered_map<uintptr_t, DescriptorInfo> m_dsv_descriptors{};
    std::vector<BindingEvent> m_recent_bindings{};
    std::vector<BarrierEvent> m_recent_barriers{};
    std::vector<WarningEvent> m_recent_warnings{};
    std::optional<CurrentBindContext> m_current_bind_context{};

    uint64_t m_frame{};
    uintptr_t m_device{};
    uintptr_t m_swapchain{};
    uintptr_t m_command_queue{};
    uint32_t m_render_width{};
    uint32_t m_render_height{};
    uint32_t m_display_width{};
    uint32_t m_display_height{};
    bool m_proton_swapchain{};
    bool m_framegen_swapchain{};
    uintptr_t m_active_cbv_srv_uav_heap{};
    uintptr_t m_active_sampler_heap{};
    uint32_t m_descriptor_heap_sets_this_frame{};
    uint32_t m_descriptor_heap_switches_this_frame{};
    uint32_t m_resource_barriers_this_frame{};
    uint32_t m_rtv_binds_this_frame{};
    uint32_t m_transient_heap_creations_this_frame{};
    uint32_t m_transient_resource_creations_this_frame{};
    uint64_t m_transient_resource_bytes_this_frame{};
};
} // namespace render
