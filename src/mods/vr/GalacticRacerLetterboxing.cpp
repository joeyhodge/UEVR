#define NOMINMAX
#include "GalacticRacerLetterboxing.hpp"

#include <atomic>
#include <chrono>
#include <windows.h>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <utility/Module.hpp>
#include <utility/Scan.hpp>
#include <sdk/FName.hpp>
#include <sdk/GalacticRacerRuntime.hpp>
#include <sdk/UObjectBase.hpp>

namespace uevr::swgr_letterbox {
namespace {
class Runtime;
std::atomic<Runtime*> published{};

bool read_local(void*, uintptr_t address, void* output, size_t size) {
    __try { std::memcpy(output, reinterpret_cast<const void*>(address), size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool store_returned_value(Chain* output, const Chain& value) {
    ULONG_PTR low{}, high{};
    GetCurrentThreadStackLimits(&low, &high);
    const auto address = reinterpret_cast<uintptr_t>(output);
    // The proven renderer callers return into their own stack. Never mutate a
    // retained/shared heap array, or use a return address after this invocation.
    if (!stack_return_buffer(address, low, high)) { return false; }
    __try { std::memcpy(output, &value, sizeof(value)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class Runtime {
public:
    void update(bool requested, bool supported) {
        if (m_shutdown.load(std::memory_order_acquire)) { return; }
        if (!requested || !supported) {
            disarm(requested ? Status::inactive_mode : Status::off);
            return;
        }
        if (!m_names.valid()) {
            if (!initialize_names()) { set_status(Status::waiting_names); return; }
        }
        if (!m_attempted) { m_attempted = true; install(); }
        if (!m_hook.enabled()) { set_status(m_install_failure); return; }
        if (!m_armed.load(std::memory_order_acquire)) {
            m_filtered_eyes.store(0, std::memory_order_relaxed);
            m_epoch.fetch_add(1, std::memory_order_release);
            set_status(Status::armed);
            m_armed.store(true, std::memory_order_release);
        }
    }

    void shutdown() {
        m_shutdown.store(true, std::memory_order_release);
        disarm(Status::off);
    }
    Status status() const { return m_status.load(std::memory_order_acquire); }

private:
    void disarm(Status state) {
        if (m_armed.exchange(false, std::memory_order_acq_rel)) { m_epoch.fetch_add(1, std::memory_order_release); }
        set_status(state);
    }
    void set_status(Status value) {
        if (m_status.exchange(value, std::memory_order_acq_rel) != value) {
            SPDLOG_INFO("[SWGR][Letterboxing] {}", status_text(value));
        }
    }
    bool initialize_names() {
        const auto now = std::chrono::steady_clock::now();
        if (now < m_next_names) { return false; }
        m_next_names = now+std::chrono::seconds(2);
        if (!sdk::UObjectBase::has_validated_header_layout() ||
            !sdk::FName::s_checked_case_preserving.load(std::memory_order_acquire) || sdk::FName::runtime_size() != 8) {
            return false;
        }
        const auto h = sdk::UObjectBase::header_layout();
        if (h.flags != 8 || h.index != 0xc || h.uclass != 0x10 || h.name != 0x18 || h.outer != 0x20 || h.packed_index) {
            return false;
        }
        // Resolve once on the game thread. Render callbacks only compare numeric
        // names and read fresh borrowed objects; they never scan or call Unreal.
        const sdk::OwnedFName dynamic_name{L"MaterialInstanceDynamic", sdk::EFindName::Find};
        const sdk::OwnedFName material_name{L"Material", sdk::EFindName::Find};
        const sdk::OwnedFName parent_name{L"M_PP_CameraLetterboxing", sdk::EFindName::Find};
        const Names names{dynamic_name.a1, material_name.a1, parent_name.a1};
        if (!names.valid() || dynamic_name.get_number() || material_name.get_number() || parent_name.get_number()) {
            return false;
        }
        m_names = names;
        return true;
    }

    void install() {
        const auto module = GetModuleHandleW(nullptr);
        m_base = reinterpret_cast<uintptr_t>(module);
        m_size = utility::get_module_size(module).value_or(0);
        const Memory memory{nullptr, read_local};
        const auto validate = [&](uintptr_t address) {
            if (!in_module(address, getter_code.size(), m_base, m_size)) { return false; }
            DWORD64 owner{};
            const auto entry = RtlLookupFunctionEntry(address, &owner, nullptr);
            std::array<uint8_t, getter_code.size()> bytes{};
            return entry && owner == m_base && m_base+entry->BeginAddress == address &&
                entry->EndAddress-entry->BeginAddress == bytes.size() &&
                memory.load(address, bytes) && code_matches(bytes);
        };
        uintptr_t candidate{};
        if (m_size && known_getter_rva <= m_size && validate(m_base+known_getter_rva)) {
            candidate = m_base+known_getter_rva;
        } else if (m_size && m_size <= (std::numeric_limits<uintptr_t>::max)()-m_base) {
            size_t matches{}, checked{};
            for (auto cursor = m_base; cursor < m_base+m_size && checked < 32;) {
                const auto hit = utility::scan(cursor, m_base+m_size-cursor, getter_signature);
                if (!hit) { break; }
                ++checked;
                cursor = *hit+1;
                if (validate(*hit)) { candidate = *hit; ++matches; }
            }
            if (matches != 1 || checked == 32) { candidate = 0; }
        }
        if (!candidate) { m_install_failure = Status::unsupported; return; }
        auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(candidate), &get_chain,
            safetyhook::InlineHook::StartDisabled);
        if (!hook) { m_install_failure = Status::hook_failed; return; }
        m_hook = std::move(*hook);
        // Publish an owned original trampoline before enabling. Checkbox/mode
        // changes only disarm; they never repeatedly remove/reinstall the hook.
        if (!m_hook.enable()) { m_install_failure = Status::hook_failed; return; }
        SPDLOG_INFO("[SWGR][Letterboxing] Validated material-chain ABI at {:x}; inline storage only", candidate);
    }

    static Chain* get_chain(Chain* output, const void* view, int32_t location) {
        auto& self = *published.load(std::memory_order_acquire);
        const bool was_armed = self.m_armed.load(std::memory_order_acquire);
        const auto epoch = self.m_epoch.load(std::memory_order_acquire);
        const auto enabled = [&] {
            return was_armed && self.m_armed.load(std::memory_order_acquire) &&
                epoch == self.m_epoch.load(std::memory_order_acquire);
        };
        return dispatch(output, view, location,
            [&](Chain* out, const void* current_view, int32_t current_location) {
                return self.m_hook.call<Chain*>(out, current_view, current_location);
            }, enabled, [&](Chain* out, const void* current_view, int32_t current_location) {
                const Memory memory{nullptr, read_local};
                const auto eye = eye_of_view(memory, reinterpret_cast<uintptr_t>(current_view));
                Chain current{};
                if (!eye || !memory.load(reinterpret_cast<uintptr_t>(out), current)) { return; }
                const auto replacement = filtered_chain(memory, current, current_location, self.m_names, self.m_base, self.m_size);
                if (!replacement || !enabled() || !store_returned_value(out, *replacement)) { return; }
                const auto bits = self.m_filtered_eyes.fetch_or(1u << *eye, std::memory_order_relaxed) | (1u << *eye);
                auto expected = Status::armed;
                if (bits == 3 && enabled() && self.m_status.compare_exchange_strong(expected, Status::active)) {
                    SPDLOG_INFO("[SWGR][Letterboxing] {}", status_text(Status::active));
                }
            });
    }

    Names m_names{};
    uintptr_t m_base{};
    size_t m_size{};
    bool m_attempted{};
    Status m_install_failure{Status::unsupported};
    std::chrono::steady_clock::time_point m_next_names{};
    std::atomic_bool m_armed{}, m_shutdown{};
    std::atomic_uint64_t m_epoch{};
    std::atomic_uint m_filtered_eyes{};
    std::atomic<Status> m_status{Status::off};
    safetyhook::InlineHook m_hook{};
};
}

void update(bool requested, bool supported) {
    auto* instance = published.load(std::memory_order_acquire);
    if (!sdk::galactic_racer::current_dx12_game()) {
        if (instance) { instance->update(false, false); }
        return;
    }
    if (!instance && !requested) { return; }
    static Runtime runtime{};
    if (!instance) { instance = &runtime; published.store(instance, std::memory_order_release); }
    instance->update(requested, supported);
}
void shutdown() { if (auto* r = published.load(std::memory_order_acquire)) { r->shutdown(); } }
Status status() { const auto* r = published.load(std::memory_order_acquire); return r ? r->status() : Status::off; }
const char* status_text(Status value) {
    switch (value) {
    case Status::off: return "Off";
    case Status::inactive_mode: return "Inactive: requires active DX12 Native / Native Fix (not Mono, Synced, or split-screen)";
    case Status::waiting_names: return "Waiting for validated object/name layout and letterbox asset";
    case Status::unsupported: return "Unsupported material-chain code; game unchanged";
    case Status::hook_failed: return "Material-chain hook unavailable; game unchanged";
    case Status::armed: return "Ready: waiting for the cinematic letterbox material";
    case Status::active: return "Active: letterbox material excluded for both eyes; other effects retained";
    }
    return "Unknown";
}
}
