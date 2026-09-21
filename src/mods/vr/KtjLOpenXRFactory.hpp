#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <sdk/BoundedDiscovery.hpp>

namespace uevr::ktjl::openxr_factory {
namespace detail {
inline constexpr uint32_t proxy_size = 0x46000;
inline constexpr uint32_t proxy_timestamp = 0x672a974b;
inline constexpr uint32_t proxy_factory_rva = 0x4976;
inline constexpr uint32_t proxy_forward_rva = 0x38958;
inline constexpr uint32_t runtime_size = 0x394000;
inline constexpr uint32_t runtime_timestamp = 0x67571d1e;
inline constexpr uint32_t factory_iat_rva = 0x295848;
inline constexpr uint32_t factory_call_rva = 0x18b7ed;
inline constexpr uint32_t factory_iid_rva = 0x2da730;
inline constexpr std::array<uint8_t, 21> factory_call{
    0x48, 0x8d, 0x54, 0x24, 0x68, 0x48, 0x8d, 0x0d, 0x37, 0xef, 0x14,
    0x00, 0xff, 0x15, 0x49, 0xa0, 0x10, 0x00, 0x44, 0x8b, 0xc0};
inline constexpr std::array<uint8_t, 16> factory_iid{
    0x78, 0xae, 0x0a, 0x77, 0x6f, 0xf2, 0xba, 0x4d,
    0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87};

bool in_scope(bool enabled, bool dx12, std::wstring_view executable, std::string_view requested_runtime);
bool same_path(std::wstring_view left, std::wstring_view right);
bool wmr_leaf_path(std::wstring_view path, std::wstring_view program_files);
bool wmr_manifest(std::string_view json, std::wstring_view system_directory);
bool validate_proxy(const sdk::discovery::Memory&, uintptr_t base, uintptr_t exported_factory,
    uintptr_t system_legacy, uintptr_t system_factory1);

struct Slot {
    uintptr_t address{};
    uintptr_t original{};
    uintptr_t replacement{};
};

// A valid, already-correct import is returned too; it is never written.
std::optional<Slot> validate_runtime(const sdk::discovery::Memory&, uintptr_t base,
    uintptr_t proxy_factory1, uintptr_t system_factory1);

struct EditApi {
    void* context{};
    bool (*protect)(void*, uintptr_t, uint32_t, uint32_t&){};
    bool (*exchange)(void*, uintptr_t, uintptr_t, uintptr_t, uintptr_t&){};
};

class Transaction {
public:
    explicit Transaction(EditApi api) : m_api{api} {}
    bool apply(std::span<const Slot> slots);
    bool rollback();
    bool clean() const;
private:
    struct Record {
        Slot slot{};
        uint32_t restore_protection{};
        bool owns_pointer{};
        bool protection_pending{};
    };
    EditApi m_api;
    std::array<Record, 2> m_records{};
    size_t m_count{};
    bool m_started{};
};

class RetryGate {
public:
    bool acquire(int32_t result, bool null_instance) {
        // XR_ERROR_RUNTIME_FAILURE only. Never retry other errors or a live instance.
        return result == -2 && null_instance && !m_used.exchange(true, std::memory_order_relaxed);
    }
private:
    std::atomic<bool> m_used{false};
};
}

// Pins validated, already-loaded runtime images before the ordinary create call.
// No import changes occur unless that call fails with XR_ERROR_RUNTIME_FAILURE.
class Repair {
public:
    Repair(bool enabled, bool dx12, std::wstring_view executable, std::string_view requested_runtime);
    ~Repair();
    Repair(const Repair&) = delete;
    Repair& operator=(const Repair&) = delete;
    bool retry_after_failure(int32_t result, bool null_instance);
    void finish(bool instance_created);
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
}
