#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <safetyhook.hpp>
#include <utility/PointerHook.hpp>

#include <uevr/PreInjectionShaderRegistry.hpp>

namespace {
constexpr size_t CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX = 10;
constexpr size_t CREATE_COMPUTE_PIPELINE_STATE_VTABLE_INDEX = 11;
constexpr size_t CREATE_PIPELINE_LIBRARY_VTABLE_INDEX = 44;
constexpr size_t CREATE_PIPELINE_STATE_VTABLE_INDEX = 47;
constexpr size_t LOAD_GRAPHICS_PIPELINE_VTABLE_INDEX = 9;
constexpr size_t LOAD_COMPUTE_PIPELINE_VTABLE_INDEX = 10;
constexpr size_t LOAD_PIPELINE_VTABLE_INDEX = 13;
constexpr uint64_t MAX_RETAINED_SHADER_BYTES = 256ull * 1024ull * 1024ull;
constexpr size_t MAX_PIPELINE_STREAM_BYTES = 16ull * 1024ull * 1024ull;
constexpr size_t MAX_RECORDS = 1'000'000;

using D3D12CreateDeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateGraphicsPipelineStateFn = HRESULT (WINAPI*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateComputePipelineStateFn = HRESULT (WINAPI*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreatePipelineLibraryFn = HRESULT (WINAPI*)(ID3D12Device1*, const void*, SIZE_T, REFIID, void**);
using CreatePipelineStateFn = HRESULT (WINAPI*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
using LoadGraphicsPipelineFn = HRESULT (WINAPI*)(
    ID3D12PipelineLibrary*, LPCWSTR, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadComputePipelineFn = HRESULT (WINAPI*)(
    ID3D12PipelineLibrary*, LPCWSTR, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using LoadPipelineFn = HRESULT (WINAPI*)(
    ID3D12PipelineLibrary1*, LPCWSTR, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

struct RegistryState {
    std::mutex mutex{};
    std::vector<UEVRShaderRegistryRecordV1> records{};
    std::unordered_map<std::string, std::vector<uint8_t>> shader_blobs{};
    std::vector<std::unique_ptr<PointerHook>> graphics_hooks{};
    std::vector<std::unique_ptr<PointerHook>> compute_hooks{};
    std::vector<std::unique_ptr<PointerHook>> create_library_hooks{};
    std::vector<std::unique_ptr<PointerHook>> stream_hooks{};
    std::vector<std::unique_ptr<PointerHook>> load_graphics_hooks{};
    std::vector<std::unique_ptr<PointerHook>> load_compute_hooks{};
    std::vector<std::unique_ptr<PointerHook>> load_stream_hooks{};
    std::unordered_map<uintptr_t, PointerHook*> graphics_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> compute_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> create_library_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> stream_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> load_graphics_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> load_compute_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> load_stream_lookup{};
    std::unordered_set<uintptr_t> hooked_slots{};
    safetyhook::InlineHook create_device_hook{};
    UEVRShaderRegistryRecordCallbackV1 callback{};
    void* callback_context{};
    HANDLE status_mapping{};
    UEVRShaderRegistryStatusV1* status{};
    std::atomic<bool> shutting_down{};
    std::atomic<uint32_t> active_hook_calls{};
};

RegistryState g_registry{};
HMODULE g_module{};

struct ActiveHookCall {
    ActiveHookCall() {
        g_registry.active_hook_calls.fetch_add(1, std::memory_order_acq_rel);
    }

    ~ActiveHookCall() {
        g_registry.active_hook_calls.fetch_sub(1, std::memory_order_acq_rel);
    }
};

std::string fnv1a(const void* data, size_t size) {
    if (data == nullptr || size == 0) {
        return {};
    }

    constexpr uint64_t offset = 1469598103934665603ull;
    constexpr uint64_t prime = 1099511628211ull;
    auto hash = offset;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= prime;
    }

    std::array<char, 17> text{};
    _snprintf_s(text.data(), text.size(), _TRUNCATE, "%016llx", static_cast<unsigned long long>(hash));
    return text.data();
}

void copy_hash(char (&destination)[17], const std::string& hash) {
    if (!hash.empty()) {
        memcpy(destination, hash.data(), (std::min)(hash.size(), sizeof(destination) - 1));
    }
}

uint64_t retain_shader(const D3D12_SHADER_BYTECODE& shader, std::string& hash_out) {
    hash_out = fnv1a(shader.pShaderBytecode, shader.BytecodeLength);
    if (hash_out.empty()) {
        return 0;
    }

    if (g_registry.shader_blobs.contains(hash_out)) {
        return 0;
    }

    auto* status = g_registry.status;
    if (status == nullptr || shader.BytecodeLength > MAX_RETAINED_SHADER_BYTES ||
        status->retained_bytes > MAX_RETAINED_SHADER_BYTES - shader.BytecodeLength) {
        if (status != nullptr) {
            status->dropped_shader_bytes += shader.BytecodeLength;
        }
        return 0;
    }

    const auto* bytes = static_cast<const uint8_t*>(shader.pShaderBytecode);
    if (bytes == nullptr) {
        return 0;
    }

    try {
        g_registry.shader_blobs.emplace(hash_out, std::vector<uint8_t>{bytes, bytes + shader.BytecodeLength});
        status->retained_bytes += shader.BytecodeLength;
        status->unique_shaders = g_registry.shader_blobs.size();
        return shader.BytecodeLength;
    } catch (...) {
        status->dropped_shader_bytes += shader.BytecodeLength;
        return 0;
    }
}

template <typename T>
struct alignas(void*) PipelineStateStreamSubobject {
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;
    T payload;
};

template <typename T>
constexpr size_t stream_payload_offset() {
    return offsetof(PipelineStateStreamSubobject<T>, payload);
}

template <typename T>
constexpr size_t stream_subobject_size() {
    return sizeof(PipelineStateStreamSubobject<T>);
}

template <typename T>
bool read_stream_payload(
    const uint8_t* stream_bytes,
    size_t stream_size,
    size_t subobject_offset,
    T& out) {
    const auto payload_offset = subobject_offset + stream_payload_offset<T>();
    const auto payload_end = subobject_offset + stream_subobject_size<T>();
    if (payload_offset > stream_size || payload_end > stream_size) {
        return false;
    }

    std::memcpy(&out, stream_bytes + payload_offset, sizeof(T));
    return true;
}

size_t pipeline_stream_subobject_size(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
        return stream_subobject_size<ID3D12RootSignature*>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS:
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS:
        return stream_subobject_size<D3D12_SHADER_BYTECODE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
        return stream_subobject_size<D3D12_STREAM_OUTPUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
        return stream_subobject_size<D3D12_BLEND_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
        return stream_subobject_size<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
        return stream_subobject_size<D3D12_RASTERIZER_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
        return stream_subobject_size<D3D12_DEPTH_STENCIL_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
        return stream_subobject_size<D3D12_INPUT_LAYOUT_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
        return stream_subobject_size<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
        return stream_subobject_size<D3D12_PRIMITIVE_TOPOLOGY_TYPE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
        return stream_subobject_size<D3D12_RT_FORMAT_ARRAY>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
        return stream_subobject_size<DXGI_FORMAT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
        return stream_subobject_size<DXGI_SAMPLE_DESC>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
        return stream_subobject_size<UINT>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
        return stream_subobject_size<D3D12_CACHED_PIPELINE_STATE>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
        return stream_subobject_size<D3D12_PIPELINE_STATE_FLAGS>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
        return stream_subobject_size<D3D12_DEPTH_STENCIL_DESC1>();
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
        return stream_subobject_size<D3D12_VIEW_INSTANCING_DESC>();
    default:
        return 0;
    }
}

bool retain_pipeline_stream_shaders(
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    UEVRShaderRegistryRecordV1& record) {
    if (desc == nullptr || desc->pPipelineStateSubobjectStream == nullptr ||
        desc->SizeInBytes == 0 || desc->SizeInBytes > MAX_PIPELINE_STREAM_BYTES) {
        return false;
    }

    const auto* stream = static_cast<const uint8_t*>(desc->pPipelineStateSubobjectStream);
    size_t offset{};
    while (offset < desc->SizeInBytes) {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type{};
        if (offset + sizeof(type) > desc->SizeInBytes) {
            return false;
        }

        std::memcpy(&type, stream + offset, sizeof(type));
        const auto subobject_size = pipeline_stream_subobject_size(type);
        if (subobject_size == 0 || subobject_size > desc->SizeInBytes - offset) {
            return false;
        }

        if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ||
            type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS) {
            D3D12_SHADER_BYTECODE shader{};
            if (!read_stream_payload(stream, desc->SizeInBytes, offset, shader)) {
                return false;
            }

            std::string hash{};
            record.retained_shader_bytes += retain_shader(shader, hash);
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS) {
                copy_hash(record.vertex_hash, hash);
            } else if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS) {
                copy_hash(record.pixel_hash, hash);
            } else {
                copy_hash(record.compute_hash, hash);
            }
        }

        offset += subobject_size;
    }

    return offset == desc->SizeInBytes;
}

void publish_graphics_pipeline(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc);
void publish_compute_pipeline(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc);
void publish_pipeline_stream(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc);

void publish_record(UEVRShaderRegistryRecordV1 record) {
    UEVRShaderRegistryRecordCallbackV1 callback{};
    void* callback_context{};
    {
        std::scoped_lock lock{g_registry.mutex};
        auto* status = g_registry.status;
        if (status == nullptr) {
            return;
        }

        record.sequence = ++status->last_sequence;
        if (g_registry.records.size() >= MAX_RECORDS) {
            ++status->dropped_records;
            return;
        }

        g_registry.records.emplace_back(record);
        status->records = g_registry.records.size();
        status->state = UEVRShaderRegistryState_Capturing;
        switch (record.kind) {
        case UEVRShaderRegistryRecord_Graphics:
            ++status->graphics_pipelines;
            break;
        case UEVRShaderRegistryRecord_Compute:
            ++status->compute_pipelines;
            break;
        case UEVRShaderRegistryRecord_PipelineStream:
            ++status->pipeline_streams;
            break;
        default:
            break;
        }
        callback = g_registry.callback;
        callback_context = g_registry.callback_context;
    }

    if (callback != nullptr) {
        callback(&record, callback_context);
    }
}

void publish_graphics_pipeline(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc) {
    if (pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    UEVRShaderRegistryRecordV1 record{};
    record.kind = UEVRShaderRegistryRecord_Graphics;
    record.device = reinterpret_cast<uintptr_t>(device);
    record.pipeline_state = reinterpret_cast<uintptr_t>(pipeline_state);
    record.descriptor_size = sizeof(*desc);
    {
        std::scoped_lock lock{g_registry.mutex};
        std::string vs_hash{};
        std::string ps_hash{};
        record.retained_shader_bytes += retain_shader(desc->VS, vs_hash);
        record.retained_shader_bytes += retain_shader(desc->PS, ps_hash);
        copy_hash(record.vertex_hash, vs_hash);
        copy_hash(record.pixel_hash, ps_hash);
    }
    publish_record(record);
}

void publish_compute_pipeline(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc) {
    if (pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    UEVRShaderRegistryRecordV1 record{};
    record.kind = UEVRShaderRegistryRecord_Compute;
    record.device = reinterpret_cast<uintptr_t>(device);
    record.pipeline_state = reinterpret_cast<uintptr_t>(pipeline_state);
    record.descriptor_size = sizeof(*desc);
    {
        std::scoped_lock lock{g_registry.mutex};
        std::string hash{};
        record.retained_shader_bytes = retain_shader(desc->CS, hash);
        copy_hash(record.compute_hash, hash);
    }
    publish_record(record);
}

void publish_pipeline_stream(
    ID3D12Device* device,
    ID3D12PipelineState* pipeline_state,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc) {
    if (pipeline_state == nullptr || desc == nullptr) {
        return;
    }

    UEVRShaderRegistryRecordV1 record{};
    record.kind = UEVRShaderRegistryRecord_PipelineStream;
    record.device = reinterpret_cast<uintptr_t>(device);
    record.pipeline_state = reinterpret_cast<uintptr_t>(pipeline_state);
    record.descriptor_size = desc->SizeInBytes;
    if (desc->pPipelineStateSubobjectStream != nullptr &&
        desc->SizeInBytes > 0 && desc->SizeInBytes <= MAX_PIPELINE_STREAM_BYTES) {
        copy_hash(record.descriptor_hash, fnv1a(desc->pPipelineStateSubobjectStream, desc->SizeInBytes));
        std::scoped_lock lock{g_registry.mutex};
        retain_pipeline_stream_shaders(desc, record);
    }
    publish_record(record);
}

PointerHook* find_hook(std::unordered_map<uintptr_t, PointerHook*>& lookup, void* slot) {
    const auto it = lookup.find(reinterpret_cast<uintptr_t>(slot));
    return it != lookup.end() ? it->second : nullptr;
}

HRESULT WINAPI create_graphics_pipeline_state(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(device))[CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX];
    CreateGraphicsPipelineStateFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.graphics_lookup, slot); hook != nullptr) {
            original = hook->get_original<CreateGraphicsPipelineStateFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);
    if (g_registry.shutting_down.load(std::memory_order_acquire)) {
        return result;
    }
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        publish_graphics_pipeline(device, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

HRESULT WINAPI create_compute_pipeline_state(
    ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(device))[CREATE_COMPUTE_PIPELINE_STATE_VTABLE_INDEX];
    CreateComputePipelineStateFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.compute_lookup, slot); hook != nullptr) {
            original = hook->get_original<CreateComputePipelineStateFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);
    if (g_registry.shutting_down.load(std::memory_order_acquire)) {
        return result;
    }
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        publish_compute_pipeline(device, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

HRESULT WINAPI create_pipeline_state(
    ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(device))[CREATE_PIPELINE_STATE_VTABLE_INDEX];
    CreatePipelineStateFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.stream_lookup, slot); hook != nullptr) {
            original = hook->get_original<CreatePipelineStateFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, desc, riid, pipeline_state);
    if (g_registry.shutting_down.load(std::memory_order_acquire)) {
        return result;
    }
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        publish_pipeline_stream(device, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

void hook_pipeline_library(IUnknown* pipeline_library);

HRESULT WINAPI create_pipeline_library(
    ID3D12Device1* device,
    const void* library_blob,
    SIZE_T blob_length,
    REFIID riid,
    void** pipeline_library) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(device))[CREATE_PIPELINE_LIBRARY_VTABLE_INDEX];
    CreatePipelineLibraryFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.create_library_lookup, slot); hook != nullptr) {
            original = hook->get_original<CreatePipelineLibraryFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(device, library_blob, blob_length, riid, pipeline_library);
    if (SUCCEEDED(result) && pipeline_library != nullptr && *pipeline_library != nullptr &&
        !g_registry.shutting_down.load(std::memory_order_acquire)) {
        hook_pipeline_library(static_cast<IUnknown*>(*pipeline_library));
    }
    return result;
}

HRESULT WINAPI load_graphics_pipeline(
    ID3D12PipelineLibrary* library,
    LPCWSTR name,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(library))[LOAD_GRAPHICS_PIPELINE_VTABLE_INDEX];
    LoadGraphicsPipelineFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.load_graphics_lookup, slot); hook != nullptr) {
            original = hook->get_original<LoadGraphicsPipelineFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(library, name, desc, riid, pipeline_state);
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        !g_registry.shutting_down.load(std::memory_order_acquire)) {
        publish_graphics_pipeline(nullptr, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

HRESULT WINAPI load_compute_pipeline(
    ID3D12PipelineLibrary* library,
    LPCWSTR name,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    REFIID riid,
    void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(library))[LOAD_COMPUTE_PIPELINE_VTABLE_INDEX];
    LoadComputePipelineFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.load_compute_lookup, slot); hook != nullptr) {
            original = hook->get_original<LoadComputePipelineFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(library, name, desc, riid, pipeline_state);
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        !g_registry.shutting_down.load(std::memory_order_acquire)) {
        publish_compute_pipeline(nullptr, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

HRESULT WINAPI load_pipeline(
    ID3D12PipelineLibrary1* library,
    LPCWSTR name,
    const D3D12_PIPELINE_STATE_STREAM_DESC* desc,
    REFIID riid,
    void** pipeline_state) {
    const ActiveHookCall active_call{};
    auto* slot = &(*reinterpret_cast<void***>(library))[LOAD_PIPELINE_VTABLE_INDEX];
    LoadPipelineFn original{};
    {
        std::scoped_lock lock{g_registry.mutex};
        if (auto* hook = find_hook(g_registry.load_stream_lookup, slot); hook != nullptr) {
            original = hook->get_original<LoadPipelineFn>();
        }
    }
    if (original == nullptr) {
        return E_FAIL;
    }

    const auto result = original(library, name, desc, riid, pipeline_state);
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState) &&
        !g_registry.shutting_down.load(std::memory_order_acquire)) {
        publish_pipeline_stream(nullptr, static_cast<ID3D12PipelineState*>(*pipeline_state), desc);
    }
    return result;
}

template <typename Fn>
void add_pointer_hook(
    IUnknown* object,
    size_t index,
    Fn hook_function,
    std::vector<std::unique_ptr<PointerHook>>& hooks,
    std::unordered_map<uintptr_t, PointerHook*>& lookup) {
    if (object == nullptr) {
        return;
    }

    auto* slot = &(*reinterpret_cast<void***>(object))[index];
    const auto key = reinterpret_cast<uintptr_t>(slot);
    if (!g_registry.hooked_slots.insert(key).second) {
        return;
    }

    auto hook = std::make_unique<PointerHook>(slot, reinterpret_cast<void*>(hook_function));
    lookup[key] = hook.get();
    hooks.emplace_back(std::move(hook));
}

void hook_pipeline_library(IUnknown* pipeline_library) {
    if (pipeline_library == nullptr || g_registry.shutting_down.load(std::memory_order_acquire)) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary> library{};
    Microsoft::WRL::ComPtr<ID3D12PipelineLibrary1> library1{};
    pipeline_library->QueryInterface(IID_PPV_ARGS(&library));
    pipeline_library->QueryInterface(IID_PPV_ARGS(&library1));

    std::scoped_lock lock{g_registry.mutex};
    if (library != nullptr) {
        add_pointer_hook(
            library.Get(),
            LOAD_GRAPHICS_PIPELINE_VTABLE_INDEX,
            &load_graphics_pipeline,
            g_registry.load_graphics_hooks,
            g_registry.load_graphics_lookup);
        add_pointer_hook(
            library.Get(),
            LOAD_COMPUTE_PIPELINE_VTABLE_INDEX,
            &load_compute_pipeline,
            g_registry.load_compute_hooks,
            g_registry.load_compute_lookup);
    }
    if (library1 != nullptr) {
        add_pointer_hook(
            library1.Get(),
            LOAD_PIPELINE_VTABLE_INDEX,
            &load_pipeline,
            g_registry.load_stream_hooks,
            g_registry.load_stream_lookup);
    }
}

void hook_device(ID3D12Device* device) {
    if (device == nullptr || g_registry.shutting_down.load()) {
        return;
    }

    std::scoped_lock lock{g_registry.mutex};
    add_pointer_hook(device, CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX, &create_graphics_pipeline_state,
        g_registry.graphics_hooks, g_registry.graphics_lookup);
    add_pointer_hook(device, CREATE_COMPUTE_PIPELINE_STATE_VTABLE_INDEX, &create_compute_pipeline_state,
        g_registry.compute_hooks, g_registry.compute_lookup);

    Microsoft::WRL::ComPtr<ID3D12Device1> device1{};
    Microsoft::WRL::ComPtr<ID3D12Device2> device2{};
    Microsoft::WRL::ComPtr<ID3D12Device3> device3{};
    Microsoft::WRL::ComPtr<ID3D12Device4> device4{};
    Microsoft::WRL::ComPtr<ID3D12Device5> device5{};
    Microsoft::WRL::ComPtr<ID3D12Device6> device6{};
    Microsoft::WRL::ComPtr<ID3D12Device7> device7{};
    Microsoft::WRL::ComPtr<ID3D12Device8> device8{};
    Microsoft::WRL::ComPtr<ID3D12Device9> device9{};
    Microsoft::WRL::ComPtr<ID3D12Device10> device10{};
    device->QueryInterface(IID_PPV_ARGS(&device1));
    device->QueryInterface(IID_PPV_ARGS(&device2));
    device->QueryInterface(IID_PPV_ARGS(&device3));
    device->QueryInterface(IID_PPV_ARGS(&device4));
    device->QueryInterface(IID_PPV_ARGS(&device5));
    device->QueryInterface(IID_PPV_ARGS(&device6));
    device->QueryInterface(IID_PPV_ARGS(&device7));
    device->QueryInterface(IID_PPV_ARGS(&device8));
    device->QueryInterface(IID_PPV_ARGS(&device9));
    device->QueryInterface(IID_PPV_ARGS(&device10));

    const std::array<IUnknown*, 10> interfaces{
        device1.Get(), device2.Get(), device3.Get(), device4.Get(), device5.Get(),
        device6.Get(), device7.Get(), device8.Get(), device9.Get(), device10.Get(),
    };
    for (auto* iface : interfaces) {
        add_pointer_hook(
            iface,
            CREATE_PIPELINE_LIBRARY_VTABLE_INDEX,
            &create_pipeline_library,
            g_registry.create_library_hooks,
            g_registry.create_library_lookup);
        add_pointer_hook(iface, CREATE_GRAPHICS_PIPELINE_STATE_VTABLE_INDEX, &create_graphics_pipeline_state,
            g_registry.graphics_hooks, g_registry.graphics_lookup);
        add_pointer_hook(iface, CREATE_COMPUTE_PIPELINE_STATE_VTABLE_INDEX, &create_compute_pipeline_state,
            g_registry.compute_hooks, g_registry.compute_lookup);
    }

    const std::array<IUnknown*, 9> stream_interfaces{
        device2.Get(), device3.Get(), device4.Get(), device5.Get(), device6.Get(),
        device7.Get(), device8.Get(), device9.Get(), device10.Get(),
    };
    for (auto* iface : stream_interfaces) {
        add_pointer_hook(iface, CREATE_PIPELINE_STATE_VTABLE_INDEX, &create_pipeline_state,
            g_registry.stream_hooks, g_registry.stream_lookup);
    }

    if (g_registry.status != nullptr) {
        g_registry.status->hooks_active = 1;
        g_registry.status->state = UEVRShaderRegistryState_Armed;
    }
}

HRESULT WINAPI d3d12_create_device(
    IUnknown* adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void** device) {
    const ActiveHookCall active_call{};
    const auto original = g_registry.create_device_hook.original<D3D12CreateDeviceFn>();
    const auto result = original(adapter, minimum_feature_level, riid, device);
    if (SUCCEEDED(result) && device != nullptr && *device != nullptr) {
        Microsoft::WRL::ComPtr<ID3D12Device> base_device{};
        if (SUCCEEDED(static_cast<IUnknown*>(*device)->QueryInterface(IID_PPV_ARGS(&base_device)))) {
            hook_device(base_device.Get());
        }
    }
    return result;
}

bool release_creation_hooks() {
    g_registry.shutting_down.store(true);

    // A later overlay may have chained above one of our vtable hooks. Removing
    // the slot cannot remove our function from that chain, so keep the hook
    // objects and original lookups alive as inert pass-throughs. UEVR can
    // safely install its hooks above them without any failed PSO creations.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (g_registry.active_hook_calls.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(1);
    }

    {
        std::scoped_lock lock{g_registry.mutex};
        if (g_registry.status != nullptr) {
            g_registry.status->hooks_active = 0;
            g_registry.status->state = UEVRShaderRegistryState_Disabled;
            if (g_registry.active_hook_calls.load(std::memory_order_acquire) != 0) {
                g_registry.status->last_error = WAIT_TIMEOUT;
            }
        }
    }
    return true;
}

bool enumerate_records(UEVRShaderRegistryRecordCallbackV1 callback, void* context) {
    if (callback == nullptr) {
        return false;
    }

    std::vector<UEVRShaderRegistryRecordV1> snapshot{};
    {
        std::scoped_lock lock{g_registry.mutex};
        snapshot = g_registry.records;
    }
    for (const auto& record : snapshot) {
        callback(&record, context);
    }
    return true;
}

bool set_record_callback(UEVRShaderRegistryRecordCallbackV1 callback, void* context) {
    std::scoped_lock lock{g_registry.mutex};
    g_registry.callback = callback;
    g_registry.callback_context = context;
    return true;
}

bool get_status(UEVRShaderRegistryStatusV1* status) {
    if (status == nullptr || status->size < sizeof(UEVRShaderRegistryStatusV1)) {
        return false;
    }
    std::scoped_lock lock{g_registry.mutex};
    if (g_registry.status == nullptr) {
        return false;
    }
    *status = *g_registry.status;
    return true;
}

const UEVRShaderRegistryApiV1 g_api{
    sizeof(UEVRShaderRegistryApiV1),
    UEVR_SHADER_REGISTRY_ABI_V1,
    &enumerate_records,
    &set_record_callback,
    &get_status,
    &release_creation_hooks,
};

void initialize_status_mapping() {
    wchar_t name[96]{};
    _snwprintf_s(name, _countof(name), _TRUNCATE, L"Local\\UEVRShaderRegistry-%lu", GetCurrentProcessId());
    g_registry.status_mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(UEVRShaderRegistryStatusV1), name);
    if (g_registry.status_mapping == nullptr) {
        return;
    }

    g_registry.status = static_cast<UEVRShaderRegistryStatusV1*>(
        MapViewOfFile(g_registry.status_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(UEVRShaderRegistryStatusV1)));
    if (g_registry.status != nullptr) {
        *g_registry.status = {};
        g_registry.status->process_id = GetCurrentProcessId();
        g_registry.status->state = UEVRShaderRegistryState_WaitingForD3D12;
    }
}

DWORD WINAPI worker_thread(void*) {
    initialize_status_mapping();
    // Explicitly loading D3D12 while the game is suspended removes the race
    // between a polling worker and the game's first D3D12CreateDevice call.
    const auto d3d12 = LoadLibraryW(L"d3d12.dll");
    if (d3d12 == nullptr) {
        if (g_registry.status != nullptr) {
            g_registry.status->state = UEVRShaderRegistryState_Error;
            g_registry.status->last_error = GetLastError();
        }
        return 0;
    }

    const auto create_device = GetProcAddress(d3d12, "D3D12CreateDevice");
    if (create_device == nullptr) {
        if (g_registry.status != nullptr) {
            g_registry.status->state = UEVRShaderRegistryState_Error;
            g_registry.status->last_error = GetLastError();
        }
        return 0;
    }

    g_registry.create_device_hook = safetyhook::create_inline(create_device, &d3d12_create_device);
    if (!g_registry.create_device_hook) {
        if (g_registry.status != nullptr) {
            g_registry.status->state = UEVRShaderRegistryState_Error;
            g_registry.status->last_error = ERROR_INVALID_HOOK_HANDLE;
        }
        return 0;
    }

    if (g_registry.status != nullptr) {
        g_registry.status->hooks_active = 1;
        g_registry.status->state = UEVRShaderRegistryState_Armed;
    }
    return 0;
}
}

extern "C" __declspec(dllexport) const UEVRShaderRegistryApiV1* UEVRShaderRegistry_GetApi(uint32_t requested_version) {
    return requested_version == UEVR_SHADER_REGISTRY_ABI_V1 ? &g_api : nullptr;
}

extern "C" __declspec(dllexport) bool UEVRShaderRegistry_Disable() {
    return release_creation_hooks();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
        if (const auto thread = CreateThread(nullptr, 0, &worker_thread, nullptr, 0, nullptr); thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        g_registry.shutting_down.store(true);
    }
    return TRUE;
}
