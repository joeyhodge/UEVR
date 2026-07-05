#include "PreInjectionShaderRegistry.hpp"

#include <Windows.h>

#include <spdlog/spdlog.h>

#include "ShaderOverrideRegistry.hpp"

namespace render {
PreInjectionShaderRegistry& PreInjectionShaderRegistry::get() {
    static PreInjectionShaderRegistry instance{};
    return instance;
}

bool PreInjectionShaderRegistry::adopt() {
    if (m_attempted.exchange(true)) {
        return m_creation_hooks_owned.load();
    }

    const auto module = GetModuleHandleW(L"UEVRShaderRegistryBootstrap.dll");
    if (module == nullptr) {
        return false;
    }

    const auto get_api = reinterpret_cast<UEVRShaderRegistryGetApiFn>(
        GetProcAddress(module, "UEVRShaderRegistry_GetApi"));
    if (get_api == nullptr) {
        spdlog::warn("[PreInjectionShaderRegistry] Helper is loaded without a compatible API export");
        return false;
    }

    m_api = get_api(UEVR_SHADER_REGISTRY_ABI_V1);
    if (m_api == nullptr || m_api->size < sizeof(UEVRShaderRegistryApiV1) ||
        m_api->abi_version != UEVR_SHADER_REGISTRY_ABI_V1) {
        spdlog::warn("[PreInjectionShaderRegistry] ABI mismatch; disabling helper hooks and using backend hooks");
        using DisableFn = bool (*)();
        if (const auto disable = reinterpret_cast<DisableFn>(
                GetProcAddress(module, "UEVRShaderRegistry_Disable")); disable != nullptr) {
            disable();
        }
        m_api = nullptr;
        return false;
    }

    if (m_api->set_record_callback != nullptr) {
        m_api->set_record_callback(&PreInjectionShaderRegistry::on_record, this);
    }
    if (m_api->enumerate_records != nullptr) {
        m_api->enumerate_records(&PreInjectionShaderRegistry::on_record, this);
    }

    UEVRShaderRegistryStatusV1 helper_status{};
    const auto status_ok = m_api->get_status != nullptr && m_api->get_status(&helper_status);
    const auto owns_hooks = status_ok && helper_status.hooks_active != 0 &&
        (helper_status.state == UEVRShaderRegistryState_Armed ||
         helper_status.state == UEVRShaderRegistryState_Capturing);
    if (!owns_hooks && m_api->release_creation_hooks != nullptr) {
        m_api->release_creation_hooks();
    }
    m_creation_hooks_owned.store(owns_hooks);

    spdlog::info(
        "[PreInjectionShaderRegistry] Adopted helper: records={} graphics={} compute={} streams={} bytes={} hooks={}",
        helper_status.records,
        helper_status.graphics_pipelines,
        helper_status.compute_pipelines,
        helper_status.pipeline_streams,
        helper_status.retained_bytes,
        owns_hooks);
    return owns_hooks;
}

bool PreInjectionShaderRegistry::creation_hooks_owned() const {
    return m_creation_hooks_owned.load();
}

UEVRShaderRegistryStatusV1 PreInjectionShaderRegistry::status() const {
    UEVRShaderRegistryStatusV1 result{};
    if (m_api != nullptr && m_api->get_status != nullptr) {
        m_api->get_status(&result);
    }
    return result;
}

void PreInjectionShaderRegistry::on_record(const UEVRShaderRegistryRecordV1* record, void* context) {
    if (record == nullptr || context == nullptr || record->size < sizeof(UEVRShaderRegistryRecordV1)) {
        return;
    }
    static_cast<PreInjectionShaderRegistry*>(context)->import_record(*record);
}

void PreInjectionShaderRegistry::import_record(const UEVRShaderRegistryRecordV1& record) {
    ShaderOverrideRegistry::get().register_preinjection_d3d12_pipeline(
        reinterpret_cast<ID3D12Device*>(record.device),
        reinterpret_cast<ID3D12PipelineState*>(record.pipeline_state),
        record.kind == UEVRShaderRegistryRecord_PipelineStream,
        record.vertex_hash,
        record.pixel_hash,
        record.compute_hash,
        record.descriptor_hash);
}
}
