#pragma once

#include <atomic>

#include <uevr/PreInjectionShaderRegistry.hpp>

namespace render {
class PreInjectionShaderRegistry {
public:
    static PreInjectionShaderRegistry& get();

    bool adopt();
    bool creation_hooks_owned() const;
    UEVRShaderRegistryStatusV1 status() const;

private:
    static void on_record(const UEVRShaderRegistryRecordV1* record, void* context);
    void import_record(const UEVRShaderRegistryRecordV1& record);

    std::atomic<bool> m_attempted{};
    std::atomic<bool> m_creation_hooks_owned{};
    const UEVRShaderRegistryApiV1* m_api{};
};
}
