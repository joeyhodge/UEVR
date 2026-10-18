#define NOMINMAX
#include "GalacticRacerPanini.hpp"

#include <atomic>
#include <chrono>
#include <windows.h>
#include <safetyhook.hpp>
#include <spdlog/spdlog.h>
#include <sdk/BoundedDiscovery.hpp>
#include <sdk/DiscoveryMemory.hpp>
#include <sdk/CVar.hpp>
#include <sdk/GalacticRacerRuntime.hpp>

namespace uevr::swgr_panini {
namespace {
class Runtime;
std::atomic<Runtime*> published{};
constexpr wchar_t cvar_name[] = L"Gfn.Panini.Enable";

bool flags_of(sdk::IConsoleVariable* variable, uint32_t& flags) {
    // UE5.7 adds SetHelp before GetFlags: do not use the older SDK virtual
    // declaration here. Decode the original slot-3 scalar accessor, never call it.
    const auto memory = sdk::discovery::process_memory();
    uintptr_t table{}, getter{};
    std::array<uint8_t, 7> code{};
    MEMORY_BASIC_INFORMATION info{};
    if (!memory.load(reinterpret_cast<uintptr_t>(variable), table) || !memory.load(table + 24, getter) ||
        !VirtualQuery(reinterpret_cast<void*>(getter), &info, sizeof(info)) || info.Type != MEM_IMAGE ||
        info.AllocationBase != GetModuleHandleW(nullptr) || !memory.executable ||
        !memory.executable(memory.context, getter, code.size()) || !memory.load(getter, code)) { return false; }
    const auto offset = flags_offset(code);
    return offset && memory.load(reinterpret_cast<uintptr_t>(variable) + *offset, flags);
}

std::optional<ConsoleValue> read_console() {
    try {
        const auto manager = sdk::FConsoleManager::get();
        const auto object = manager ? manager->find(cvar_name) : nullptr;
        if (!object || !object->is_validated_ue57_variable() || object->AsCommand() != nullptr) { return {}; }
        auto* variable = static_cast<sdk::IConsoleVariable*>(object);
        const auto value = variable->TryGetInt();
        uint32_t flags{};
        if (!value || !flags_of(variable, flags)) { return {}; }
        ConsoleValue result{reinterpret_cast<uintptr_t>(variable), flags, *value};
        return result.valid() ? std::optional{result} : std::nullopt;
    } catch (...) { return {}; }
}

void write_console(const ConsoleValue& expected, int value) {
    // Re-find before writes: never call an old address after console replacement.
    const auto current = read_console();
    if (!current || current->object != expected.object || current->flags != expected.flags ||
        current->value != expected.value) { return; }
    auto* variable = reinterpret_cast<sdk::IConsoleVariable*>(current->object);
    try { variable->Set(value ? L"1" : L"0", current->flags & 0xff000000u); }
    catch (...) { /* The lease verifies readback and retains pending restoration. */ }
}

class Runtime {
public:
    void update(bool cvar, bool component, bool active) {
        m_armed.store(false, std::memory_order_release);
        const bool conflict = cvar && component;
        const bool want_cvar = active && cvar && !component;
        const bool want_component = active && exclusive_component(cvar, component);
        if (want_cvar != m_cvar_requested) {
            m_cvar_requested = want_cvar;
            m_failures = 0;
            m_blocked = false;
            m_next = {};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= m_next && !m_blocked) {
            m_next = now + std::chrono::seconds(2);
            const auto result = want_cvar ? m_lease.apply(read_console, write_console) : m_lease.restore(read_console, write_console);
            set_status(m_cvar_status, conflict && !m_lease.pending() ? Status::conflicting_options : result, "CVar");
            if (result == Status::external_change) { m_blocked = true; }
            else if (result == Status::unavailable || result == Status::restore_pending) { m_blocked = ++m_failures >= 8; }
            else { m_failures = 0; }
        }
        if (!want_component) {
            m_armed.store(false, std::memory_order_release);
            m_tick_count.store(0, std::memory_order_release);
            set_status(m_component_status, conflict ? Status::conflicting_options : Status::off, "Component");
            m_component_attempted = false;
            return;
        }
        if (m_lease.pending()) {
            m_armed.store(false, std::memory_order_release);
            set_status(m_component_status, Status::restore_pending, "Component");
            return;
        }
        if (!m_hook.enabled() && !m_component_attempted) {
            m_component_attempted = true;
            install_component();
        }
        m_game_thread.store(GetCurrentThreadId(), std::memory_order_release);
        m_armed.store(m_hook.enabled(), std::memory_order_release);
        if (m_hook.enabled()) {
            set_status(m_component_status, m_tick_count.load(std::memory_order_acquire) ? Status::active : Status::waiting, "Component");
        }
    }

    void disarm() { m_armed.store(false, std::memory_order_release); }
    Status cvar_status() const { return m_cvar_status.load(std::memory_order_acquire); }
    Status component_status() const { return m_component_status.load(std::memory_order_acquire); }

private:
    static void set_status(std::atomic<Status>& slot, Status value, const char* path) {
        if (slot.exchange(value, std::memory_order_acq_rel) != value) {
            SPDLOG_INFO("[SWGR][Panini][{}] {}", path, status_text(value));
        }
    }
    void install_component() {
        if (m_hook.enabled()) { return; }
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto memory = sdk::discovery::process_memory();
        const auto read = [&](uint32_t rva, std::span<uint8_t> bytes) {
            return memory.executable && memory.executable(memory.context, base + rva, bytes.size()) &&
                memory.read(memory.context, base + rva, bytes.data(), bytes.size());
        };
        const auto size = [&](uint32_t rva) -> size_t {
            DWORD64 owner{};
            const auto entry = RtlLookupFunctionEntry(base + rva, &owner, nullptr);
            return entry && owner == base && entry->BeginAddress == rva && entry->EndAddress > rva
                ? entry->EndAddress - rva : 0;
        };
        if (!component_contract(read, size)) {
            set_status(m_component_status, Status::unsupported, "Component");
            return;
        }
        auto hook = safetyhook::InlineHook::create(reinterpret_cast<void*>(base + component_witnesses[0].rva),
            &component_tick, safetyhook::InlineHook::StartDisabled);
        if (!hook) { set_status(m_component_status, Status::hook_failed, "Component"); return; }
        m_hook = std::move(*hook);
        // Publish the original before the callback can run. Disabled hooks keep
        // their trampoline until shutdown; checkbox changes never churn hooks.
        if (!m_hook.enable()) { set_status(m_component_status, Status::hook_failed, "Component"); return; }
        set_status(m_component_status, Status::waiting, "Component");
    }
    static void component_tick(void* component, float delta, uint32_t tick_type, void* tick_function) {
        auto& self = *published.load(std::memory_order_acquire);
        const bool active = suppression_allowed(self.m_armed.load(std::memory_order_acquire),
            self.m_game_thread.load(std::memory_order_acquire), GetCurrentThreadId(), reinterpret_cast<uintptr_t>(component));
        if (!active) { self.m_hook.call<void>(component, delta, tick_type, tick_function); return; }
        // The exact original body proves this byte and the four-argument Tick
        // ABI. The engine owns the live component for the duration of its Tick.
        auto& flag = *(static_cast<uint8_t*>(component) + temporary_camera_flag);
        if (flag > 1) { self.m_hook.call<void>(component, delta, tick_type, tick_function); return; }
        TickSuppression suppression{flag, true};
        self.m_hook.call<void>(component, delta, tick_type, tick_function);
        self.m_tick_count.fetch_add(1, std::memory_order_release);
    }

    ConsoleLease m_lease{};
    bool m_cvar_requested{}, m_blocked{}, m_component_attempted{};
    uint32_t m_failures{};
    std::chrono::steady_clock::time_point m_next{};
    std::atomic<Status> m_cvar_status{Status::off}, m_component_status{Status::off};
    std::atomic_bool m_armed{};
    std::atomic<DWORD> m_game_thread{};
    std::atomic<uint64_t> m_tick_count{};
    safetyhook::InlineHook m_hook{};
};
}

void update(bool cvar, bool component, bool active) {
    // No console lookup, scan, hook or singleton creation in other games or
    // while both default-off options have never been used.
    if (!sdk::galactic_racer::current_dx12_game()) {
        if (auto* instance = published.load(std::memory_order_acquire)) { instance->update(false, false, false); }
        return;
    }
    auto* instance = published.load(std::memory_order_acquire);
    if (!instance && !cvar && !component) { return; }
    static Runtime runtime{};
    if (!instance) { instance = &runtime; published.store(instance, std::memory_order_release); }
    instance->update(cvar, component, active);
}
void shutdown() { if (auto* instance = published.load(std::memory_order_acquire)) { instance->disarm(); } }
Status cvar_status() { const auto* r = published.load(std::memory_order_acquire); return r ? r->cvar_status() : Status::off; }
Status component_status() { const auto* r = published.load(std::memory_order_acquire); return r ? r->component_status() : Status::off; }
const char* status_text(Status status) {
    switch (status) {
    case Status::off: return "Off";
    case Status::waiting: return "Armed; waiting for the game's component Tick";
    case Status::active: return "Active";
    case Status::already_off: return "Game Panini is already disabled";
    case Status::unavailable: return "CVar unavailable, read-only, or write not accepted";
    case Status::external_change: return "Changed externally; no overwrite (toggle off/on to retry)";
    case Status::restore_pending: return "Waiting to safely restore the previous CVar value";
    case Status::unsupported: return "Unsupported component code; game unchanged";
    case Status::hook_failed: return "Component hook unavailable; game unchanged";
    case Status::conflicting_options: return "Enable only one Panini test; both paths are inactive";
    }
    return "Unknown";
}
}
