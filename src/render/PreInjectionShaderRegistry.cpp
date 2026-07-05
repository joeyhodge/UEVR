#include "PreInjectionShaderRegistry.hpp"

#include <Windows.h>

#include <algorithm>
#include <string>

#include <spdlog/spdlog.h>

#include "ShaderOverrideRegistry.hpp"

namespace render {
namespace {
std::string bounded_hash(const char (&hash)[17]) {
    const auto end = std::find(hash, hash + 17, '\0');
    if (end == hash + 17 || end - hash > 16) {
        return {};
    }

    std::string result{hash, end};
    if (!std::all_of(result.begin(), result.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
        return {};
    }
    return result;
}
}

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
    if (record.kind < UEVRShaderRegistryRecord_Graphics ||
        record.kind > UEVRShaderRegistryRecord_PipelineStream ||
        record.pipeline_state < 0x10000 ||
        (record.pipeline_state & (alignof(void*) - 1)) != 0) {
        return;
    }

    ShaderOverrideRegistry::get().register_preinjection_d3d12_pipeline(
        nullptr,
        reinterpret_cast<ID3D12PipelineState*>(record.pipeline_state),
        record.kind == UEVRShaderRegistryRecord_PipelineStream,
        bounded_hash(record.vertex_hash),
        bounded_hash(record.pixel_hash),
        bounded_hash(record.compute_hash),
        bounded_hash(record.descriptor_hash));
}
}
