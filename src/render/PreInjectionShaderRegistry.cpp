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

    UEVRShaderRegistryStatusV1 helper_status{};
    const auto status_ok = m_api->get_status != nullptr && m_api->get_status(&helper_status);
    const auto helper_owns_hooks = status_ok && helper_status.hooks_active != 0 &&
        (helper_status.state == UEVRShaderRegistryState_Armed ||
         helper_status.state == UEVRShaderRegistryState_Capturing);

    // The bootstrap DLL is intentionally startup-only. Leaving its device
    // vtable hooks installed alongside UEVR's D3D12 hooks can delay discovery
    // of the real device/swapchain and prevent the backbuffer from stabilizing.
    const auto released_hooks =
        m_api->release_creation_hooks != nullptr && m_api->release_creation_hooks();
    auto owns_hooks = helper_owns_hooks && !released_hooks;
    if (owns_hooks && m_api->set_record_callback != nullptr) {
        // Fall back to the old ownership model if an older helper cannot hand
        // its hooks back safely.
        m_api->set_record_callback(&PreInjectionShaderRegistry::on_record, this);
    }
    m_creation_hooks_owned.store(owns_hooks);

    if (released_hooks) {
        spdlog::info(
            "[PreInjectionShaderRegistry] Registered dormant startup records and handed D3D12 creation back to backend hooks: "
            "records={} graphics={} compute={} streams={} bytes={}",
            helper_status.records,
            helper_status.graphics_pipelines,
            helper_status.compute_pipelines,
            helper_status.pipeline_streams,
            helper_status.retained_bytes);
    } else {
        spdlog::warn(
            "[PreInjectionShaderRegistry] Registered dormant startup records but helper hook handoff failed; "
            "records={} hooks={}",
            helper_status.records,
            owns_hooks);
    }
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

size_t PreInjectionShaderRegistry::imported_record_count() const {
    return m_imported_records.load(std::memory_order_acquire);
}

void PreInjectionShaderRegistry::consume_for_diagnostics(size_t max_records) {
    if (m_api == nullptr || max_records == 0) {
        return;
    }

    bool expected = false;
    if (m_snapshot_loaded.compare_exchange_strong(expected, true)) {
        const auto loaded = m_api->enumerate_records != nullptr &&
            m_api->enumerate_records(&PreInjectionShaderRegistry::on_record, this);
        if (!loaded) {
            m_snapshot_loaded.store(false);
            return;
        }

        std::scoped_lock lock{m_records_mutex};
        spdlog::info(
            "[PreInjectionShaderRegistry] Shader diagnostics requested; queued {} startup records for bounded import",
            m_records.size());
    }

    std::vector<UEVRShaderRegistryRecordV1> batch{};
    bool finished = false;
    {
        std::scoped_lock lock{m_records_mutex};
        const auto start = (std::min)(m_next_record, m_records.size());
        const auto remaining = m_records.size() - start;
        const auto count = (std::min)(remaining, max_records);
        batch.insert(
            batch.end(),
            m_records.begin() + start,
            m_records.begin() + start + count);
        m_next_record = start + count;
        finished = m_next_record == m_records.size() && count != 0;
    }

    for (const auto& record : batch) {
        import_record(record);
    }

    if (finished) {
        spdlog::info("[PreInjectionShaderRegistry] Finished bounded startup-record import");
    }
}

void PreInjectionShaderRegistry::on_record(const UEVRShaderRegistryRecordV1* record, void* context) {
    if (record == nullptr || context == nullptr || record->size < sizeof(UEVRShaderRegistryRecordV1)) {
        return;
    }
    static_cast<PreInjectionShaderRegistry*>(context)->queue_record(*record);
}

void PreInjectionShaderRegistry::queue_record(const UEVRShaderRegistryRecordV1& record) {
    if (record.kind < UEVRShaderRegistryRecord_Graphics ||
        record.kind > UEVRShaderRegistryRecord_PipelineStream ||
        record.pipeline_state < 0x10000 ||
        (record.pipeline_state & (alignof(void*) - 1)) != 0) {
        return;
    }

    std::scoped_lock lock{m_records_mutex};
    m_records.emplace_back(record);
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
    m_imported_records.fetch_add(1, std::memory_order_release);
}
}
