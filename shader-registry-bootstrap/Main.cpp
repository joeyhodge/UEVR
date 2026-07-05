#include <Windows.h>

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
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
constexpr size_t CREATE_PIPELINE_STATE_VTABLE_INDEX = 47;
constexpr uint64_t MAX_RETAINED_SHADER_BYTES = 256ull * 1024ull * 1024ull;
constexpr size_t MAX_RECORDS = 1'000'000;

using D3D12CreateDeviceFn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using CreateGraphicsPipelineStateFn = HRESULT (WINAPI*)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
using CreateComputePipelineStateFn = HRESULT (WINAPI*)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
using CreatePipelineStateFn = HRESULT (WINAPI*)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

struct RegistryState {
    std::mutex mutex{};
    std::vector<UEVRShaderRegistryRecordV1> records{};
    std::unordered_map<std::string, std::vector<uint8_t>> shader_blobs{};
    std::vector<std::unique_ptr<PointerHook>> graphics_hooks{};
    std::vector<std::unique_ptr<PointerHook>> compute_hooks{};
    std::vector<std::unique_ptr<PointerHook>> stream_hooks{};
    std::unordered_map<uintptr_t, PointerHook*> graphics_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> compute_lookup{};
    std::unordered_map<uintptr_t, PointerHook*> stream_lookup{};
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
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        UEVRShaderRegistryRecordV1 record{};
        record.kind = UEVRShaderRegistryRecord_Graphics;
        record.device = reinterpret_cast<uintptr_t>(device);
        record.pipeline_state = reinterpret_cast<uintptr_t>(*pipeline_state);
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
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        UEVRShaderRegistryRecordV1 record{};
        record.kind = UEVRShaderRegistryRecord_Compute;
        record.device = reinterpret_cast<uintptr_t>(device);
        record.pipeline_state = reinterpret_cast<uintptr_t>(*pipeline_state);
        record.descriptor_size = sizeof(*desc);
        {
            std::scoped_lock lock{g_registry.mutex};
            std::string hash{};
            record.retained_shader_bytes = retain_shader(desc->CS, hash);
            copy_hash(record.compute_hash, hash);
        }
        publish_record(record);
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
    if (SUCCEEDED(result) && desc != nullptr && pipeline_state != nullptr && *pipeline_state != nullptr &&
        riid == __uuidof(ID3D12PipelineState)) {
        UEVRShaderRegistryRecordV1 record{};
        record.kind = UEVRShaderRegistryRecord_PipelineStream;
        record.device = reinterpret_cast<uintptr_t>(device);
        record.pipeline_state = reinterpret_cast<uintptr_t>(*pipeline_state);
        record.descriptor_size = desc->SizeInBytes;
        copy_hash(record.descriptor_hash, fnv1a(desc->pPipelineStateSubobjectStream, desc->SizeInBytes));
        publish_record(record);
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
    {
        std::scoped_lock lock{g_registry.mutex};
        if (g_registry.create_device_hook) {
            (void)g_registry.create_device_hook.disable();
        }
        for (const auto& hook : g_registry.graphics_hooks) {
            hook->remove();
        }
        for (const auto& hook : g_registry.compute_hooks) {
            hook->remove();
        }
        for (const auto& hook : g_registry.stream_hooks) {
            hook->remove();
        }
    }

    // Let calls that entered before the slots were restored finish before the
    // frontend injects UEVR and installs the backend's own D3D12 hooks.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (g_registry.active_hook_calls.load(std::memory_order_acquire) != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(1);
    }

    {
        std::scoped_lock lock{g_registry.mutex};
        g_registry.create_device_hook.reset();
        g_registry.graphics_hooks.clear();
        g_registry.compute_hooks.clear();
        g_registry.stream_hooks.clear();
        g_registry.graphics_lookup.clear();
        g_registry.compute_lookup.clear();
        g_registry.stream_lookup.clear();
        g_registry.hooked_slots.clear();
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
