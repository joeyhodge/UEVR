#pragma once

#include <cstddef>
#include <cstdint>

#define UEVR_SHADER_REGISTRY_ABI_V1 1u
#define UEVR_SHADER_REGISTRY_STATUS_MAGIC 0x52534855u

enum UEVRShaderRegistryRecordKind : uint32_t {
    UEVRShaderRegistryRecord_Graphics = 1,
    UEVRShaderRegistryRecord_Compute = 2,
    UEVRShaderRegistryRecord_PipelineStream = 3,
};

enum UEVRShaderRegistryState : uint32_t {
    UEVRShaderRegistryState_Starting = 0,
    UEVRShaderRegistryState_WaitingForD3D12 = 1,
    UEVRShaderRegistryState_Armed = 2,
    UEVRShaderRegistryState_Capturing = 3,
    UEVRShaderRegistryState_Disabled = 4,
    UEVRShaderRegistryState_Error = 5,
};

struct UEVRShaderRegistryRecordV1 {
    uint32_t size{sizeof(UEVRShaderRegistryRecordV1)};
    uint32_t kind{};
    uint64_t sequence{};
    uintptr_t device{};
    uintptr_t pipeline_state{};
    uint64_t descriptor_size{};
    uint64_t retained_shader_bytes{};
    char vertex_hash[17]{};
    char pixel_hash[17]{};
    char compute_hash[17]{};
    char descriptor_hash[17]{};
};

struct UEVRShaderRegistryStatusV1 {
    uint32_t magic{UEVR_SHADER_REGISTRY_STATUS_MAGIC};
    uint32_t size{sizeof(UEVRShaderRegistryStatusV1)};
    uint32_t abi_version{UEVR_SHADER_REGISTRY_ABI_V1};
    uint32_t state{UEVRShaderRegistryState_Starting};
    uint32_t process_id{};
    uint32_t hooks_active{};
    uint64_t records{};
    uint64_t graphics_pipelines{};
    uint64_t compute_pipelines{};
    uint64_t pipeline_streams{};
    uint64_t unique_shaders{};
    uint64_t retained_bytes{};
    uint64_t dropped_records{};
    uint64_t dropped_shader_bytes{};
    uint64_t last_sequence{};
    uint32_t last_error{};
    uint32_t reserved{};
};

using UEVRShaderRegistryRecordCallbackV1 = void (*)(const UEVRShaderRegistryRecordV1* record, void* context);

struct UEVRShaderRegistryApiV1 {
    uint32_t size{sizeof(UEVRShaderRegistryApiV1)};
    uint32_t abi_version{UEVR_SHADER_REGISTRY_ABI_V1};
    bool (*enumerate_records)(UEVRShaderRegistryRecordCallbackV1 callback, void* context){};
    bool (*set_record_callback)(UEVRShaderRegistryRecordCallbackV1 callback, void* context){};
    bool (*get_status)(UEVRShaderRegistryStatusV1* status){};
    bool (*release_creation_hooks)(){};
};

using UEVRShaderRegistryGetApiFn = const UEVRShaderRegistryApiV1* (*)(uint32_t requested_version);

