#include "render/D3D12Diagnostics.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <sstream>
#include <string_view>

namespace {
constexpr size_t MAX_RECENT_BINDINGS = 96;
constexpr size_t MAX_RECENT_BARRIERS = 128;
constexpr size_t MAX_RECENT_WARNINGS = 64;
constexpr size_t MAX_RECENT_COPY_EVENTS = 128;
constexpr size_t MAX_RECENT_EYE_MISMATCHES = 32;

template <typename T>
void push_ring(std::vector<T>& values, T value, size_t max_entries) {
    if (values.size() >= max_entries) {
        values.erase(values.begin());
    }

    values.emplace_back(std::move(value));
}

std::string format_pointer(uintptr_t pointer) {
    std::ostringstream ss{};
    ss << "0x" << std::hex << std::uppercase << pointer;
    return ss.str();
}

std::string descriptor_heap_type_to_string(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        return "CBV/SRV/UAV";
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
        return "Sampler";
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return "RTV";
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return "DSV";
    default:
        return "Unknown";
    }
}

std::string barrier_type_to_string(D3D12_RESOURCE_BARRIER_TYPE type) {
    switch (type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        return "Transition";
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        return "Aliasing";
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        return "UAV";
    default:
        return "Unknown";
    }
}

void append_state_token(std::string& out, const char* token) {
    if (!out.empty()) {
        out += "|";
    }

    out += token;
}

std::string resource_state_to_string(D3D12_RESOURCE_STATES state) {
    if (state == D3D12_RESOURCE_STATE_COMMON) {
        return "COMMON";
    }

    std::string result{};

    if ((state & D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) != 0) {
        append_state_token(result, "VB/CB");
    }
    if ((state & D3D12_RESOURCE_STATE_INDEX_BUFFER) != 0) {
        append_state_token(result, "IB");
    }
    if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0) {
        append_state_token(result, "RT");
    }
    if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0) {
        append_state_token(result, "UAV");
    }
    if ((state & D3D12_RESOURCE_STATE_DEPTH_WRITE) != 0) {
        append_state_token(result, "DepthWrite");
    }
    if ((state & D3D12_RESOURCE_STATE_DEPTH_READ) != 0) {
        append_state_token(result, "DepthRead");
    }
    if ((state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0) {
        append_state_token(result, "NPSR");
    }
    if ((state & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0) {
        append_state_token(result, "PSR");
    }
    if ((state & D3D12_RESOURCE_STATE_STREAM_OUT) != 0) {
        append_state_token(result, "StreamOut");
    }
    if ((state & D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) != 0) {
        append_state_token(result, "Indirect");
    }
    if ((state & D3D12_RESOURCE_STATE_COPY_DEST) != 0) {
        append_state_token(result, "CopyDest");
    }
    if ((state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0) {
        append_state_token(result, "CopySource");
    }
    if ((state & D3D12_RESOURCE_STATE_RESOLVE_DEST) != 0) {
        append_state_token(result, "ResolveDest");
    }
    if ((state & D3D12_RESOURCE_STATE_RESOLVE_SOURCE) != 0) {
        append_state_token(result, "ResolveSource");
    }
    if ((state & D3D12_RESOURCE_STATE_PRESENT) != 0) {
        append_state_token(result, "Present");
    }
    if ((state & D3D12_RESOURCE_STATE_PREDICATION) != 0) {
        append_state_token(result, "Predication");
    }

    if (result.empty()) {
        result = format_pointer(static_cast<uintptr_t>(state));
    }

    return result;
}

uint64_t bytes_per_pixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return 8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return 4;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
        return 1;
    default:
        return 4;
    }
}

uint64_t approximate_resource_size(const D3D12_RESOURCE_DESC& desc) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        return 0;
    }

    const auto samples = std::max<UINT>(1, desc.SampleDesc.Count);
    const auto array_size = std::max<UINT16>(1, desc.DepthOrArraySize);
    const auto bpp = bytes_per_pixel(desc.Format);
    return static_cast<uint64_t>(desc.Width) * static_cast<uint64_t>(std::max<UINT>(1, desc.Height)) * bpp * samples * array_size;
}

void append_note(std::string& note, std::string_view addition) {
    if (!note.empty()) {
        note += "; ";
    }

    note += addition;
}

std::string resource_name_or_pointer(ID3D12Object* object, uintptr_t pointer) {
    if (object != nullptr) {
        UINT chars = 0;
        if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &chars, nullptr)) && chars > sizeof(wchar_t)) {
            std::wstring name(chars / sizeof(wchar_t), L'\0');
            if (SUCCEEDED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &chars, name.data()))) {
                name.resize((chars / sizeof(wchar_t)) - 1);
                if (!name.empty()) {
                    std::string narrow{};
                    narrow.reserve(name.size());
                    for (const auto ch : name) {
                        narrow.push_back(static_cast<char>(ch & 0xFF));
                    }
                    return narrow;
                }
            }
        }
    }

    return format_pointer(pointer);
}
} // namespace

namespace render {
D3D12Diagnostics& D3D12Diagnostics::get() {
    static D3D12Diagnostics instance{};
    return instance;
}

void D3D12Diagnostics::set_enabled(bool enabled) {
    const auto was_enabled = m_enabled.exchange(enabled, std::memory_order_relaxed);

    if (was_enabled == enabled) {
        return;
    }

    std::scoped_lock _{m_mutex};
    clear_state_locked();
}

bool D3D12Diagnostics::is_enabled() const {
    return m_enabled.load(std::memory_order_relaxed);
}

void D3D12Diagnostics::begin_frame(
    ID3D12Device* device,
    IDXGISwapChain3* swapchain,
    ID3D12CommandQueue* queue,
    uint32_t render_width,
    uint32_t render_height,
    uint32_t display_width,
    uint32_t display_height,
    bool proton_swapchain,
    bool framegen_swapchain,
    std::string_view framegen_kind,
    uintptr_t framegen_wrapper_swapchain,
    uintptr_t framegen_internal_swapchain,
    uint32_t framegen_internal_swapchain_offset,
    bool streamline_module_loaded,
    bool streamline_link_hooked
) {
    if (!is_enabled()) {
        return;
    }

    std::scoped_lock _{m_mutex};
    if (m_eye_health_frame != 0) {
        finalize_eye_frame_locked(m_eye_health_frame);
    }

    ++m_frame;
    m_device = reinterpret_cast<uintptr_t>(device);
    m_swapchain = reinterpret_cast<uintptr_t>(swapchain);
    m_command_queue = reinterpret_cast<uintptr_t>(queue);
    m_render_width = render_width;
    m_render_height = render_height;
    m_display_width = display_width;
    m_display_height = display_height;
    m_proton_swapchain = proton_swapchain;
    m_framegen_swapchain = framegen_swapchain;
    m_framegen_kind = framegen_kind.empty() ? "none" : std::string{framegen_kind};
    m_framegen_wrapper_swapchain = framegen_wrapper_swapchain;
    m_framegen_internal_swapchain = framegen_internal_swapchain;
    m_framegen_internal_swapchain_offset = framegen_internal_swapchain_offset;
    m_streamline_module_loaded = streamline_module_loaded;
    m_streamline_link_hooked = streamline_link_hooked;
    m_descriptor_heap_sets_this_frame = 0;
    m_descriptor_heap_switches_this_frame = 0;
    m_resource_barriers_this_frame = 0;
    m_rtv_binds_this_frame = 0;
    m_transient_heap_creations_this_frame = 0;
    m_transient_resource_creations_this_frame = 0;
    m_transient_resource_bytes_this_frame = 0;

    for (auto& [_, heap] : m_heaps) {
        heap.is_active = false;
    }
}

void D3D12Diagnostics::register_descriptor_heap(
    std::string_view source,
    ID3D12DescriptorHeap* heap,
    uint32_t estimated_in_use,
    bool transient,
    std::string_view name
) {
    if (!is_enabled()) {
        return;
    }

    if (heap == nullptr) {
        push_warning(source, "Attempted to register a null descriptor heap");
        return;
    }

    std::scoped_lock _{m_mutex};

    const auto key = reinterpret_cast<uintptr_t>(heap);
    const auto desc = heap->GetDesc();
    auto it = m_heaps.find(key);
    const bool is_new = it == m_heaps.end();

    if (is_new) {
        HeapInfo info{};
        info.pointer = key;
        info.name = name.empty() ? resource_name_or_pointer(heap, key) : std::string{name};
        info.source = std::string{source};
        info.type = descriptor_heap_type_to_string(desc.Type);
        info.total_descriptors = desc.NumDescriptors;
        const auto requested_in_use = estimated_in_use == 0 ? desc.NumDescriptors : estimated_in_use;
        info.estimated_in_use = requested_in_use < desc.NumDescriptors ? requested_in_use : desc.NumDescriptors;
        info.first_seen_frame = m_frame;
        info.last_seen_frame = m_frame;
        info.shader_visible = desc.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        info.transient = transient;
        m_heaps.emplace(key, std::move(info));

        if (transient) {
            ++m_transient_heap_creations_this_frame;
        }
    } else {
        auto& info = it->second;
        info.last_seen_frame = m_frame;
        if (!name.empty()) {
            info.name = std::string{name};
        }
        info.source = std::string{source};
        info.total_descriptors = desc.NumDescriptors;
        if (estimated_in_use != 0) {
            const auto max_in_use = info.estimated_in_use > estimated_in_use ? info.estimated_in_use : estimated_in_use;
            info.estimated_in_use = max_in_use < desc.NumDescriptors ? max_in_use : desc.NumDescriptors;
        } else if (info.estimated_in_use == 0) {
            info.estimated_in_use = desc.NumDescriptors;
        }
        info.shader_visible = desc.Flags == D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        info.transient = info.transient || transient;
    }
}

void D3D12Diagnostics::register_resource(
    std::string_view source,
    ID3D12Resource* resource,
    bool transient,
    std::string_view name
) {
    if (!is_enabled()) {
        return;
    }

    if (resource == nullptr) {
        push_warning(source, "Attempted to register a null resource");
        return;
    }

    std::scoped_lock _{m_mutex};

    const auto key = reinterpret_cast<uintptr_t>(resource);
    const auto desc = resource->GetDesc();
    const auto bytes = approximate_resource_size(desc);
    auto it = m_resources.find(key);

    if (it == m_resources.end()) {
        ResourceInfo info{};
        info.pointer = key;
        info.name = name.empty() ? resource_name_or_pointer(resource, key) : std::string{name};
        info.source = std::string{source};
        info.format = std::to_string(static_cast<uint32_t>(desc.Format));
        info.width = static_cast<uint32_t>(desc.Width);
        info.height = desc.Height;
        info.approx_bytes = bytes;
        info.first_seen_frame = m_frame;
        info.last_seen_frame = m_frame;
        info.transient = transient;
        m_resources.emplace(key, std::move(info));

        if (transient) {
            ++m_transient_resource_creations_this_frame;
            m_transient_resource_bytes_this_frame += bytes;
        }
    } else {
        auto& info = it->second;
        info.last_seen_frame = m_frame;
        if (!name.empty()) {
            info.name = std::string{name};
        }
        info.source = std::string{source};
        info.format = std::to_string(static_cast<uint32_t>(desc.Format));
        info.width = static_cast<uint32_t>(desc.Width);
        info.height = desc.Height;
        info.approx_bytes = bytes;
        info.transient = info.transient || transient;
    }
}

void D3D12Diagnostics::register_rtv_descriptor(
    std::string_view source,
    ID3D12Resource* resource,
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    std::string_view name
) {
    if (!is_enabled()) {
        return;
    }

    if (handle.ptr == 0) {
        push_warning(source, "Attempted to register a null RTV descriptor");
        return;
    }

    std::scoped_lock _{m_mutex};

    if (resource != nullptr) {
        register_resource(source, resource, false, name);
    }

    const auto key = static_cast<uintptr_t>(handle.ptr);
    auto& descriptor = m_rtv_descriptors[key];
    descriptor.handle = key;
    descriptor.resource = reinterpret_cast<uintptr_t>(resource);
    descriptor.source = std::string{source};
    descriptor.descriptor_type = "RTV";
    descriptor.last_seen_frame = m_frame;

    if (descriptor.first_seen_frame == 0) {
        descriptor.first_seen_frame = m_frame;
    }

    if (resource != nullptr) {
        const auto resource_key = reinterpret_cast<uintptr_t>(resource);
        if (const auto it = m_resources.find(resource_key); it != m_resources.end()) {
            descriptor.name = it->second.name;
        }
    }

    if (descriptor.name.empty()) {
        descriptor.name = name.empty() ? format_pointer(key) : std::string{name};
    }
}

void D3D12Diagnostics::register_dsv_descriptor(
    std::string_view source,
    ID3D12Resource* resource,
    D3D12_CPU_DESCRIPTOR_HANDLE handle,
    std::string_view name
) {
    if (!is_enabled()) {
        return;
    }

    if (handle.ptr == 0) {
        push_warning(source, "Attempted to register a null DSV descriptor");
        return;
    }

    std::scoped_lock _{m_mutex};

    if (resource != nullptr) {
        register_resource(source, resource, false, name);
    }

    const auto key = static_cast<uintptr_t>(handle.ptr);
    auto& descriptor = m_dsv_descriptors[key];
    descriptor.handle = key;
    descriptor.resource = reinterpret_cast<uintptr_t>(resource);
    descriptor.source = std::string{source};
    descriptor.descriptor_type = "DSV";
    descriptor.last_seen_frame = m_frame;

    if (descriptor.first_seen_frame == 0) {
        descriptor.first_seen_frame = m_frame;
    }

    if (resource != nullptr) {
        const auto resource_key = reinterpret_cast<uintptr_t>(resource);
        if (const auto it = m_resources.find(resource_key); it != m_resources.end()) {
            descriptor.name = it->second.name;
        }
    }

    if (descriptor.name.empty()) {
        descriptor.name = name.empty() ? format_pointer(key) : std::string{name};
    }
}

void D3D12Diagnostics::record_descriptor_heaps_set(
    std::string_view source,
    uint32_t count,
    ID3D12DescriptorHeap* const* heaps
) {
    if (!is_enabled()) {
        return;
    }

    std::scoped_lock _{m_mutex};
    ++m_descriptor_heap_sets_this_frame;

    std::ostringstream detail{};
    detail << "count=" << count;

    for (uint32_t i = 0; i < count; ++i) {
        const auto heap = heaps != nullptr ? heaps[i] : nullptr;
        if (heap == nullptr) {
            push_warning(source, "SetDescriptorHeaps received a null heap");
            continue;
        }

        const auto desc = heap->GetDesc();
        register_descriptor_heap(source, heap, desc.NumDescriptors, false);

        const auto key = reinterpret_cast<uintptr_t>(heap);
        auto& tracked = m_heaps[key];
        tracked.bind_count++;
        tracked.is_active = true;

        uintptr_t* active_ptr = nullptr;
        if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            active_ptr = &m_active_cbv_srv_uav_heap;
        } else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
            active_ptr = &m_active_sampler_heap;
        }

        if (active_ptr != nullptr && *active_ptr != key) {
            if (*active_ptr != 0) {
                ++m_descriptor_heap_switches_this_frame;
            }
            *active_ptr = key;
        }

        detail << " [" << i << "] " << descriptor_heap_type_to_string(desc.Type) << "=" << format_pointer(key);
    }

    push_ring(m_recent_bindings, BindingEvent{m_frame, std::string{source}, "SetDescriptorHeaps", detail.str()}, MAX_RECENT_BINDINGS);
    note_frame_warning_if_needed();
}

void D3D12Diagnostics::record_resource_barriers(
    std::string_view source,
    uint32_t count,
    const D3D12_RESOURCE_BARRIER* barriers
) {
    if (!is_enabled()) {
        return;
    }

    if (barriers == nullptr || count == 0) {
        return;
    }

    std::scoped_lock _{m_mutex};
    m_resource_barriers_this_frame += count;

    for (uint32_t i = 0; i < count; ++i) {
        const auto& barrier = barriers[i];
        BarrierEvent event{};
        event.frame = m_frame;
        event.source = std::string{source};
        event.type = barrier_type_to_string(barrier.Type);

        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            event.resource = reinterpret_cast<uintptr_t>(barrier.Transition.pResource);
            event.before_state = resource_state_to_string(barrier.Transition.StateBefore);
            event.after_state = resource_state_to_string(barrier.Transition.StateAfter);
            event.subresource = barrier.Transition.Subresource;

            if (barrier.Transition.pResource == nullptr) {
                event.note = "null resource";
                push_warning(source, "Transition barrier with null resource");
            } else if (barrier.Transition.StateBefore == barrier.Transition.StateAfter) {
                event.note = "before == after";
                push_warning(source, "Transition barrier keeps the same before/after state");
            }
        } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
            event.resource = reinterpret_cast<uintptr_t>(barrier.Aliasing.pResourceAfter);
            event.note = "aliasing";
        } else if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
            event.resource = reinterpret_cast<uintptr_t>(barrier.UAV.pResource);
            event.note = "uav";
        }

        push_ring(m_recent_barriers, std::move(event), MAX_RECENT_BARRIERS);
    }

    note_frame_warning_if_needed();
}

void D3D12Diagnostics::record_rtv_bind(
    std::string_view source,
    uint32_t rtv_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsv
) {
    if (!is_enabled()) {
        return;
    }

    std::scoped_lock _{m_mutex};
    ++m_rtv_binds_this_frame;

    CurrentBindContext context{};
    context.frame = m_frame;
    context.source = std::string{source};
    context.exact_this_frame = true;
    context.render_targets.reserve(rtv_count);

    std::ostringstream detail{};
    detail << "rtv_count=" << rtv_count;

    if (rtvs != nullptr) {
        for (uint32_t i = 0; i < rtv_count; ++i) {
            detail << " rtv[" << i << "]=0x" << std::hex << std::uppercase << rtvs[i].ptr << std::dec;

            BoundTargetInfo target{};
            target.handle = static_cast<uintptr_t>(rtvs[i].ptr);
            target.descriptor_type = "RTV";

            if (const auto it = m_rtv_descriptors.find(target.handle); it != m_rtv_descriptors.end()) {
                target.resource = it->second.resource;
                target.name = it->second.name;
            }

            if (target.name.empty()) {
                target.name = format_pointer(target.handle);
            }

            context.render_targets.emplace_back(std::move(target));
        }
    }

    if (dsv != nullptr) {
        detail << " dsv=0x" << std::hex << std::uppercase << dsv->ptr << std::dec;

        BoundTargetInfo target{};
        target.handle = static_cast<uintptr_t>(dsv->ptr);
        target.descriptor_type = "DSV";

        if (const auto it = m_dsv_descriptors.find(target.handle); it != m_dsv_descriptors.end()) {
            target.resource = it->second.resource;
            target.name = it->second.name;
        }

        if (target.name.empty()) {
            target.name = format_pointer(target.handle);
        }

        context.depth_target = std::move(target);
    }

    m_current_bind_context = std::move(context);
    push_ring(m_recent_bindings, BindingEvent{m_frame, std::string{source}, "OMSetRenderTargets", detail.str()}, MAX_RECENT_BINDINGS);
}

void D3D12Diagnostics::record_texture_copy(
    std::string_view source,
    uint32_t swapchain_index,
    std::string_view swapchain_name,
    ID3D12Resource* src,
    ID3D12Resource* dst,
    D3D12_RESOURCE_STATES src_state,
    D3D12_RESOURCE_STATES dst_state,
    const D3D12_BOX* src_box,
    bool scene,
    bool ui,
    bool depth
) {
    if (!is_enabled()) {
        return;
    }

    TextureCopyEvent event{};
    event.source = std::string{source};
    event.swapchain_name = std::string{swapchain_name};
    event.swapchain_index = swapchain_index;
    event.src_resource = reinterpret_cast<uintptr_t>(src);
    event.dst_resource = reinterpret_cast<uintptr_t>(dst);
    event.src_state = resource_state_to_string(src_state);
    event.dst_state = resource_state_to_string(dst_state);
    event.scene = scene;
    event.ui = ui;
    event.depth = depth;

    if (src != nullptr) {
        const auto desc = src->GetDesc();
        event.src_width = static_cast<uint32_t>(desc.Width);
        event.src_height = desc.Height;
        event.src_format = static_cast<uint32_t>(desc.Format);
    } else {
        append_note(event.note, "null source");
    }

    if (dst != nullptr) {
        const auto desc = dst->GetDesc();
        event.dst_width = static_cast<uint32_t>(desc.Width);
        event.dst_height = desc.Height;
        event.dst_format = static_cast<uint32_t>(desc.Format);
    } else {
        append_note(event.note, "null destination");
    }

    if (src_box != nullptr) {
        event.has_src_box = true;
        event.box_left = src_box->left;
        event.box_top = src_box->top;
        event.box_right = src_box->right;
        event.box_bottom = src_box->bottom;

        if (src_box->right <= src_box->left || src_box->bottom <= src_box->top) {
            append_note(event.note, "empty source box");
        }

        if (event.src_width != 0 && src_box->right > event.src_width) {
            append_note(event.note, "source box exceeds width");
        }

        if (event.src_height != 0 && src_box->bottom > event.src_height) {
            append_note(event.note, "source box exceeds height");
        }
    }

    std::scoped_lock _{m_mutex};
    event.frame = m_frame;
    push_ring(m_recent_copy_events, event, MAX_RECENT_COPY_EVENTS);
    update_eye_health_locked(event);
}

D3D12Diagnostics::Snapshot D3D12Diagnostics::snapshot() const {
    if (!is_enabled()) {
        return {};
    }

    std::scoped_lock _{m_mutex};

    Snapshot out{};
    out.available = m_device != 0 || m_swapchain != 0;
    out.frame = m_frame;
    out.device = m_device;
    out.swapchain = m_swapchain;
    out.command_queue = m_command_queue;
    out.render_width = m_render_width;
    out.render_height = m_render_height;
    out.display_width = m_display_width;
    out.display_height = m_display_height;
    out.proton_swapchain = m_proton_swapchain;
    out.framegen_swapchain = m_framegen_swapchain;
    out.framegen_kind = m_framegen_kind;
    out.framegen_wrapper_swapchain = m_framegen_wrapper_swapchain;
    out.framegen_internal_swapchain = m_framegen_internal_swapchain;
    out.framegen_internal_swapchain_offset = m_framegen_internal_swapchain_offset;
    out.streamline_module_loaded = m_streamline_module_loaded;
    out.streamline_link_hooked = m_streamline_link_hooked;
    out.active_cbv_srv_uav_heap = m_active_cbv_srv_uav_heap;
    out.active_sampler_heap = m_active_sampler_heap;
    out.descriptor_heap_sets_this_frame = m_descriptor_heap_sets_this_frame;
    out.descriptor_heap_switches_this_frame = m_descriptor_heap_switches_this_frame;
    out.resource_barriers_this_frame = m_resource_barriers_this_frame;
    out.rtv_binds_this_frame = m_rtv_binds_this_frame;
    out.transient_heap_creations_this_frame = m_transient_heap_creations_this_frame;
    out.transient_resource_creations_this_frame = m_transient_resource_creations_this_frame;
    out.transient_resource_bytes_this_frame = m_transient_resource_bytes_this_frame;
    out.current_bind_context = m_current_bind_context;

    out.heaps.reserve(m_heaps.size());
    for (const auto& [_, heap] : m_heaps) {
        out.heaps.emplace_back(heap);
    }

    std::sort(out.heaps.begin(), out.heaps.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.is_active != rhs.is_active) {
            return lhs.is_active > rhs.is_active;
        }
        if (lhs.last_seen_frame != rhs.last_seen_frame) {
            return lhs.last_seen_frame > rhs.last_seen_frame;
        }
        return lhs.name < rhs.name;
    });

    out.recent_bindings = m_recent_bindings;
    out.recent_barriers = m_recent_barriers;
    out.recent_warnings = m_recent_warnings;
    out.recent_copy_events = m_recent_copy_events;
    out.recent_eye_mismatches = m_recent_eye_mismatches;

    for (const auto& [_, resource] : m_resources) {
        out.tracked_resource_bytes_total += resource.approx_bytes;
        if (resource.transient) {
            out.tracked_transient_resource_bytes_total += resource.approx_bytes;
        }
    }

    return out;
}

std::optional<D3D12Diagnostics::CurrentBindContext> D3D12Diagnostics::current_bind_context() const {
    if (!is_enabled()) {
        return std::nullopt;
    }

    std::scoped_lock _{m_mutex};
    return m_current_bind_context;
}

void D3D12Diagnostics::reset() {
    std::scoped_lock _{m_mutex};
    clear_state_locked();
}

void D3D12Diagnostics::update_eye_health_locked(const TextureCopyEvent& event) {
    if (!event.scene || event.depth) {
        return;
    }

    if (m_eye_health_frame != 0 && event.frame != m_eye_health_frame) {
        finalize_eye_frame_locked(m_eye_health_frame);
    }

    m_eye_health_frame = event.frame;

    if (event.swapchain_name == "AFR_LEFT_EYE") {
        m_current_left_eye_copy = event;
    } else if (event.swapchain_name == "AFR_RIGHT_EYE") {
        m_current_right_eye_copy = event;
    } else if (event.swapchain_name == "DOUBLE_WIDE") {
        m_current_double_wide_copy = event;
    }

    if (m_current_left_eye_copy.has_value() && m_current_right_eye_copy.has_value()) {
        finalize_eye_frame_locked(event.frame);
    }
}

void D3D12Diagnostics::finalize_eye_frame_locked(uint64_t frame) {
    auto push_mismatch = [this, frame](std::string kind, std::string message) {
        EyeMismatchEvent mismatch{};
        mismatch.frame = frame;
        mismatch.kind = std::move(kind);
        mismatch.message = std::move(message);
        if (m_current_left_eye_copy.has_value()) {
            mismatch.left = *m_current_left_eye_copy;
        }
        if (m_current_right_eye_copy.has_value()) {
            mismatch.right = *m_current_right_eye_copy;
        }
        if (m_current_double_wide_copy.has_value()) {
            mismatch.double_wide = *m_current_double_wide_copy;
        }
        push_ring(m_recent_eye_mismatches, std::move(mismatch), MAX_RECENT_EYE_MISMATCHES);
    };

    if (m_current_left_eye_copy.has_value() && m_current_right_eye_copy.has_value()) {
        const auto& left = *m_current_left_eye_copy;
        const auto& right = *m_current_right_eye_copy;

        if ((left.src_resource == 0) != (right.src_resource == 0)) {
            push_mismatch("scene_eye_source_null_mismatch", "one AFR eye copied from a null scene source");
        } else if ((left.note.empty()) != (right.note.empty())) {
            push_mismatch("scene_eye_note_mismatch", "one AFR eye copy reported a validation note");
        } else if (left.has_src_box != right.has_src_box) {
            push_mismatch("scene_eye_box_mode_mismatch", "one AFR eye used a source box and the other did not");
        } else if (left.has_src_box && right.has_src_box) {
            const auto left_width = left.box_right > left.box_left ? left.box_right - left.box_left : 0;
            const auto right_width = right.box_right > right.box_left ? right.box_right - right.box_left : 0;
            const auto left_height = left.box_bottom > left.box_top ? left.box_bottom - left.box_top : 0;
            const auto right_height = right.box_bottom > right.box_top ? right.box_bottom - right.box_top : 0;

            if (left_width != right_width || left_height != right_height) {
                push_mismatch("scene_eye_box_size_mismatch", "AFR eye source boxes have different dimensions");
            }
        } else if (left.src_format != 0 && right.src_format != 0 && left.src_format != right.src_format) {
            push_mismatch("scene_eye_format_mismatch", "AFR eyes copied from different source formats");
        }
    } else if ((m_current_left_eye_copy.has_value() || m_current_right_eye_copy.has_value()) &&
               !m_current_double_wide_copy.has_value()) {
        push_mismatch("incomplete_scene_eye_pair", "only one AFR scene eye copy was observed for this frame");
    }

    if (m_current_double_wide_copy.has_value()) {
        const auto& double_wide = *m_current_double_wide_copy;
        if (double_wide.src_resource == 0 || !double_wide.note.empty()) {
            push_mismatch("double_wide_copy_validation", "double-wide scene copy has a null source or validation note");
        }
    }

    m_current_left_eye_copy.reset();
    m_current_right_eye_copy.reset();
    m_current_double_wide_copy.reset();
    m_eye_health_frame = 0;
}

void D3D12Diagnostics::clear_state_locked() {
    m_heaps.clear();
    m_resources.clear();
    m_rtv_descriptors.clear();
    m_dsv_descriptors.clear();
    m_recent_bindings.clear();
    m_recent_barriers.clear();
    m_recent_warnings.clear();
    m_recent_copy_events.clear();
    m_recent_eye_mismatches.clear();
    m_current_bind_context.reset();
    m_current_left_eye_copy.reset();
    m_current_right_eye_copy.reset();
    m_current_double_wide_copy.reset();
    m_frame = 0;
    m_eye_health_frame = 0;
    m_device = 0;
    m_swapchain = 0;
    m_command_queue = 0;
    m_render_width = 0;
    m_render_height = 0;
    m_display_width = 0;
    m_display_height = 0;
    m_proton_swapchain = false;
    m_framegen_swapchain = false;
    m_framegen_kind.clear();
    m_framegen_wrapper_swapchain = 0;
    m_framegen_internal_swapchain = 0;
    m_framegen_internal_swapchain_offset = 0;
    m_streamline_module_loaded = false;
    m_streamline_link_hooked = false;
    m_active_cbv_srv_uav_heap = 0;
    m_active_sampler_heap = 0;
    m_descriptor_heap_sets_this_frame = 0;
    m_descriptor_heap_switches_this_frame = 0;
    m_resource_barriers_this_frame = 0;
    m_rtv_binds_this_frame = 0;
    m_transient_heap_creations_this_frame = 0;
    m_transient_resource_creations_this_frame = 0;
    m_transient_resource_bytes_this_frame = 0;
}

void D3D12Diagnostics::push_warning(std::string_view source, std::string message) {
    push_ring(m_recent_warnings, WarningEvent{m_frame, std::string{source}, std::move(message)}, MAX_RECENT_WARNINGS);
}

void D3D12Diagnostics::note_frame_warning_if_needed() {
    if (m_descriptor_heap_switches_this_frame == 32) {
        push_warning("Frame", "Descriptor heap switching exceeded 32 changes this frame");
    }

    if (m_transient_heap_creations_this_frame == 16) {
        push_warning("Frame", "Transient heap creation exceeded 16 heaps this frame");
    }

    if (m_transient_resource_bytes_this_frame > (256ull * 1024ull * 1024ull)) {
        push_warning("Frame", "Transient DX12 resource bytes exceeded 256 MB this frame");
    }
}
} // namespace render
