#pragma once

#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

#include <uevr/PreInjectionShaderRegistry.hpp>

namespace render {
class PreInjectionShaderRegistry {
public:
    static PreInjectionShaderRegistry& get();

    bool adopt();
    bool creation_hooks_owned() const;
    UEVRShaderRegistryStatusV1 status() const;
    void consume_for_diagnostics(size_t max_records = 256);

private:
    static void on_record(const UEVRShaderRegistryRecordV1* record, void* context);
    void queue_record(const UEVRShaderRegistryRecordV1& record);
    void import_record(const UEVRShaderRegistryRecordV1& record);

    std::atomic<bool> m_attempted{};
    std::atomic<bool> m_creation_hooks_owned{};
    std::atomic<bool> m_snapshot_loaded{};
    std::mutex m_records_mutex{};
    std::vector<UEVRShaderRegistryRecordV1> m_records{};
    size_t m_next_record{};
    const UEVRShaderRegistryApiV1* m_api{};
};
}
