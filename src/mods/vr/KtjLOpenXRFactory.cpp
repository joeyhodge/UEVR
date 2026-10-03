#define NOMINMAX
#include "KtjLOpenXRFactory.hpp"

#include <windows.h>
#include <psapi.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <sdk/DiscoveryMemory.hpp>
#include <sdk/KtjLObjectLayout.hpp>

namespace uevr::ktjl::openxr_factory {
namespace detail {
namespace {
bool extent(uint32_t rva, size_t length, uint32_t size) {
    return rva != 0 && rva < size && length <= size - rva;
}

std::optional<IMAGE_NT_HEADERS64> image_header(const sdk::discovery::Memory& memory,
    uintptr_t base, uint32_t size, uint32_t timestamp) {
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (base < 0x10000 || base > (std::numeric_limits<uintptr_t>::max)() - size ||
        !memory.load(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        dos.e_lfanew > 0x1000 || !memory.load(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.FileHeader.TimeDateStamp != timestamp ||
        nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.OptionalHeader.SizeOfImage != size ||
        nt.OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        nt.OptionalHeader.SizeOfHeaders > size || nt.OptionalHeader.SizeOfHeaders <
            dos.e_lfanew + sizeof(nt) + nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER)) {
        return std::nullopt;
    }
    return nt;
}

std::optional<std::string> image_string(const sdk::discovery::Memory& memory, uintptr_t base,
    uint32_t rva, uint32_t size) {
    std::string value;
    for (uint32_t i = 0; i < 128; ++i) {
        char c{};
        if (!extent(rva, i + 1, size) || !memory.load(base + rva + i, c)) { return std::nullopt; }
        if (!c) { return value; }
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) { return std::nullopt; }
        value.push_back(c);
    }
    return std::nullopt;
}

std::optional<uintptr_t> factory_import(const sdk::discovery::Memory& memory, uintptr_t base,
    const IMAGE_NT_HEADERS64& nt) {
    const auto dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.Size > 0x10000 || !extent(dir.VirtualAddress, dir.Size, runtime_size)) { return std::nullopt; }
    std::optional<uintptr_t> found;
    bool saw_dxgi{};
    bool terminated{};
    for (uint32_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.Size;
         offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR descriptor{};
        if (!memory.load(base + dir.VirtualAddress + offset, descriptor)) { return std::nullopt; }
        if (!descriptor.Name && !descriptor.OriginalFirstThunk && !descriptor.FirstThunk &&
            !descriptor.TimeDateStamp && !descriptor.ForwarderChain) { terminated = true; break; }
        const auto name = image_string(memory, base, descriptor.Name, runtime_size);
        if (!name) { return std::nullopt; }
        if (_stricmp(name->c_str(), "dxgi.dll") != 0) { continue; }
        if (saw_dxgi) { return std::nullopt; }
        saw_dxgi = true;
        bool thunk_end{};
        for (uint32_t i = 0; i < 512; ++i) {
            const size_t bytes = (i + 1) * sizeof(uintptr_t);
            uintptr_t name_thunk{}, value{};
            if (!extent(descriptor.OriginalFirstThunk, bytes, runtime_size) ||
                !extent(descriptor.FirstThunk, bytes, runtime_size) ||
                !memory.load(base + descriptor.OriginalFirstThunk + i * sizeof(uintptr_t), name_thunk) ||
                !memory.load(base + descriptor.FirstThunk + i * sizeof(uintptr_t), value)) { return std::nullopt; }
            if (!name_thunk) { thunk_end = value == 0; break; }
            if (IMAGE_SNAP_BY_ORDINAL64(name_thunk)) { continue; }
            if (name_thunk > runtime_size - sizeof(WORD)) { return std::nullopt; }
            const auto symbol = image_string(memory, base, static_cast<uint32_t>(name_thunk + sizeof(WORD)), runtime_size);
            if (!symbol) { return std::nullopt; }
            if (*symbol == "CreateDXGIFactory1") {
                if (found) { return std::nullopt; }
                found = base + descriptor.FirstThunk + i * sizeof(uintptr_t);
            }
        }
        if (!thunk_end) { return std::nullopt; }
    }
    return terminated ? found : std::nullopt;
}
}

bool in_scope(bool enabled, bool dx12, std::wstring_view executable, std::string_view requested_runtime) {
    return enabled && dx12 && requested_runtime == "openxr_loader.dll" && sdk::ktjl::matches_executable(executable);
}

bool same_path(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || right.empty() || left.size() > 32767 || right.size() > 32767) { return false; }
    const auto a = std::filesystem::path{left}.lexically_normal().native();
    const auto b = std::filesystem::path{right}.lexically_normal().native();
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool wmr_leaf_path(std::wstring_view path, std::wstring_view program_files) {
    if (program_files.empty()) { return false; }
    const auto root = std::filesystem::path{program_files} / L"WindowsApps";
    return same_path(path, (root / L"Microsoft.WindowsMixedReality.Runtime_113.2412.9001.0_x64__8wekyb3d8bbwe" /
        L"x64" / L"WinXrRuntime.dll").native()) ||
        same_path(path, (root / L"Microsoft.WindowsMixedReality.PreviewRuntime_113.2412.9001.0_x64__8wekyb3d8bbwe" /
        L"x64" / L"WinXrRuntime.dll").native());
}

bool wmr_manifest(std::string_view json, std::wstring_view system_directory) {
    if (json.empty() || json.size() > 16384 || system_directory.empty()) { return false; }
    const auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || doc.value("file_format_version", nlohmann::json{}) != "1.0.0" ||
        !doc.contains("runtime") || !doc["runtime"].is_object()) { return false; }
    const auto& runtime = doc["runtime"];
    if (runtime.contains("functions") || !runtime.contains("library_path") || !runtime["library_path"].is_string()) { return false; }
    const auto path = std::filesystem::u8path(runtime["library_path"].get<std::string>());
    const auto system = std::filesystem::path{system_directory};
    return same_path((path.is_absolute() ? path : system / path).native(), (system / L"MixedRealityRuntime.dll").native());
}

bool validate_proxy(const sdk::discovery::Memory& memory, uintptr_t base, uintptr_t exported_factory,
    uintptr_t system_legacy, uintptr_t system_factory1) {
    constexpr std::array<uint8_t, 6> thunk{0xff, 0x25, 0xdc, 0x3f, 0x03, 0x00};
    uintptr_t target{};
    return system_legacy != system_factory1 && system_legacy >= 0x10000 && system_factory1 >= 0x10000 &&
        image_header(memory, base, proxy_size, proxy_timestamp) && exported_factory == base + proxy_factory_rva &&
        sdk::ktjl::matches_code(memory, exported_factory, thunk) &&
        memory.load(base + proxy_forward_rva, target) && target == system_legacy;
}

std::optional<Slot> validate_runtime(const sdk::discovery::Memory& memory, uintptr_t base,
    uintptr_t proxy_factory1, uintptr_t system_factory1) {
    const auto nt = image_header(memory, base, runtime_size, runtime_timestamp);
    if (!nt || proxy_factory1 < 0x10000 || system_factory1 < 0x10000 || proxy_factory1 == system_factory1 ||
        !sdk::ktjl::matches_code(memory, base + factory_call_rva, factory_call)) { return std::nullopt; }
    std::array<uint8_t, 16> iid{};
    const auto slot = factory_import(memory, base, *nt);
    uintptr_t value{};
    if (!slot || *slot != base + factory_iat_rva || *slot % alignof(uintptr_t) ||
        !memory.load(base + factory_iid_rva, iid) || iid != factory_iid || !memory.load(*slot, value) ||
        (value != proxy_factory1 && value != system_factory1)) { return std::nullopt; }
    return Slot{*slot, value, system_factory1};
}

bool Transaction::clean() const {
    for (size_t i = 0; i < m_count; ++i) {
        if (m_records[i].owns_pointer || m_records[i].protection_pending) { return false; }
    }
    return true;
}

bool Transaction::apply(std::span<const Slot> slots) {
    if (m_started || !m_api.protect || !m_api.exchange || slots.empty() || slots.size() > m_records.size()) { return false; }
    m_started = true;
    // Reject the entire plan before making any writable page or changing a slot.
    for (size_t i = 0; i < slots.size(); ++i) {
        const auto& slot = slots[i];
        if (slot.address < 0x10000 || slot.address % alignof(uintptr_t) || slot.original < 0x10000 || slot.replacement < 0x10000) { return false; }
        for (size_t j = 0; j < i; ++j) { if (slots[j].address == slot.address) { return false; } }
    }
    for (const auto& slot : slots) {
        if (slot.original == slot.replacement) { continue; }
        auto& record = m_records[m_count++];
        record.slot = slot;
        uint32_t old{};
        if (!m_api.protect(m_api.context, slot.address, PAGE_READWRITE, old)) { rollback(); return false; }
        record.restore_protection = old;
        record.protection_pending = true;
        uintptr_t observed{};
        const bool changed = (old == PAGE_READONLY || old == PAGE_READWRITE) &&
            m_api.exchange(m_api.context, slot.address, slot.original, slot.replacement, observed) && observed == slot.original;
        record.owns_pointer = changed;
        uint32_t ignored{};
        const bool restored = m_api.protect(m_api.context, slot.address, old, ignored);
        record.protection_pending = !restored;
        if (!changed || !restored) { rollback(); return false; }
    }
    return !clean();
}

bool Transaction::rollback() {
    for (size_t i = m_count; i != 0; --i) {
        auto& record = m_records[i - 1];
        if (!record.owns_pointer && !record.protection_pending) { continue; }
        uint32_t old{};
        if (!m_api.protect(m_api.context, record.slot.address, PAGE_READWRITE, old)) { continue; }
        if (!record.protection_pending) { record.restore_protection = old; }
        record.protection_pending = true;
        uintptr_t observed{};
        if (record.owns_pointer && m_api.exchange(m_api.context, record.slot.address,
                record.slot.replacement, record.slot.original, observed)) {
            // A different writer now owns the slot: do not overwrite its value.
            record.owns_pointer = false;
        }
        uint32_t ignored{};
        if (m_api.protect(m_api.context, record.slot.address, record.restore_protection, ignored)) { record.protection_pending = false; }
    }
    return clean();
}
}

namespace {
constexpr auto label = "[KTJL][OpenXR][FactoryRepair]";
detail::RetryGate retry_gate;

std::wstring module_path(HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    return count && count < path.size() ? std::wstring{path.data(), count} : std::wstring{};
}

std::wstring system_directory() {
    std::array<wchar_t, MAX_PATH> path{};
    const auto count = GetSystemDirectoryW(path.data(), static_cast<UINT>(path.size()));
    return count && count < path.size() ? std::wstring{path.data(), count} : std::wstring{};
}

bool selected_wmr(const std::wstring& system) {
    // Match the embedded loader's Windows selection without modifying it. Overrides
    // and non-WMR runtimes are deliberately unsupported, even if WMR DLLs are present.
    if (system.empty() || GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0) != 0) { return false; }
    std::array<wchar_t, 32768> value{};
    DWORD bytes = sizeof(value), type{};
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
            RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, &type, value.data(), &bytes) != ERROR_SUCCESS) { return false; }
    const auto manifest = std::filesystem::path{system} / L"MixedRealityRuntime.json";
    if (!detail::same_path(value.data(), manifest.native())) { return false; }
    std::ifstream stream{manifest, std::ios::binary};
    std::array<char, 16385> contents{};
    stream.read(contents.data(), contents.size());
    const auto count = stream.gcount();
    return stream.eof() && count > 0 && count <= 16384 &&
        detail::wmr_manifest({contents.data(), static_cast<size_t>(count)}, system);
}

bool image_address(HMODULE module, uintptr_t address, bool executable) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.AllocationBase != module || region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) { return false; }
    return !executable || sdk::discovery::executable_image(nullptr, address, 1);
}

bool protect_slot(void*, uintptr_t address, uint32_t desired, uint32_t& old) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &region, sizeof(region)) != sizeof(region) ||
        region.State != MEM_COMMIT || region.Type != MEM_IMAGE ||
        (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE) ||
        region.RegionSize < sizeof(uintptr_t) ||
        address - reinterpret_cast<uintptr_t>(region.BaseAddress) > region.RegionSize - sizeof(uintptr_t)) { return false; }
    DWORD previous{};
    if (!VirtualProtect(reinterpret_cast<void*>(address), sizeof(uintptr_t), desired, &previous)) { return false; }
    old = previous;
    return true;
}

bool exchange_slot(void*, uintptr_t address, uintptr_t expected, uintptr_t desired, uintptr_t& observed) {
    __try {
        observed = reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(
            reinterpret_cast<void* volatile*>(address), reinterpret_cast<void*>(desired), reinterpret_cast<void*>(expected)));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

struct Modules {
    std::array<HMODULE, 5> refs{};
    size_t count{};
    ~Modules() { for (size_t i = count; i != 0; --i) { FreeLibrary(refs[i - 1]); } }
    bool keep(HMODULE module) {
        if (count == refs.size()) { return false; }
        refs[count++] = module;
        return true;
    }
    void retain_for_process() {
        // At most five references, once per process. The IAT points directly to
        // System32, never to a wrapper in the potentially unloadable backend.
        count = 0;
    }
};
}

struct Repair::Impl {
    Modules modules;
    detail::Transaction transaction{{nullptr, protect_slot, exchange_slot}};
    std::array<HMODULE, 2> leaves{};
    std::array<detail::Slot, 2> slots{};
    size_t leaf_count{};
    HMODULE proxy{}, system_dxgi{}, bridge{};
    uintptr_t proxy_factory{}, legacy_factory{}, correct_factory{};
    std::wstring system;
    const char* rejection = "preparation incomplete";
    bool armed{}, applied{}, finished{};

    ~Impl() {
        if (finished) { return; }
        if (!transaction.rollback()) {
            modules.retain_for_process();
            spdlog::error("{} Rollback incomplete; module references retained. Restart the game before another test.", label);
        }
    }

    bool validate() {
        const auto memory = sdk::discovery::process_memory();
        rejection = "selected runtime is not the standard WMR manifest, or an override is present";
        if (!selected_wmr(system)) { return false; }
        rejection = "proxy export or legacy forwarder does not match";
        if (!detail::validate_proxy(memory, reinterpret_cast<uintptr_t>(proxy), proxy_factory, legacy_factory, correct_factory)) { return false; }
        rejection = "factory exports do not belong to executable System32 DXGI memory";
        if (!image_address(system_dxgi, legacy_factory, true) || !image_address(system_dxgi, correct_factory, true)) { return false; }
        bool needs_repair{};
        for (size_t i = 0; i < leaf_count; ++i) {
            const auto slot = detail::validate_runtime(memory, reinterpret_cast<uintptr_t>(leaves[i]), proxy_factory, correct_factory);
            rejection = "WMR image, callsite, IID, import name, or slot owner does not match";
            if (!slot || !image_address(leaves[i], slot->address, false)) { return false; }
            MEMORY_BASIC_INFORMATION region{};
            rejection = "WMR IAT page has an unexpected protection";
            if (VirtualQuery(reinterpret_cast<void*>(slot->address), &region, sizeof(region)) != sizeof(region) ||
                (region.Protect != PAGE_READONLY && region.Protect != PAGE_READWRITE)) { return false; }
            slots[i] = *slot;
            needs_repair |= slot->original != slot->replacement;
        }
        rejection = "no incorrect WMR Factory1 import (already corrected or unavailable)";
        return leaf_count != 0 && needs_repair;
    }

    bool prepare(std::wstring_view executable) {
        system = system_directory();
        rejection = "selected runtime is not the standard WMR manifest, or an override is present";
        if (!selected_wmr(system)) { return false; }
        rejection = "KTJL executable image does not match the validated build";
        if (!sdk::ktjl::validated_image(sdk::discovery::process_memory(), reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)))) { return false; }
        std::array<wchar_t, 32768> program_files{};
        const auto length = GetEnvironmentVariableW(L"ProgramW6432", program_files.data(), static_cast<DWORD>(program_files.size()));
        rejection = "Program Files root unavailable";
        if (!length || length >= program_files.size()) { return false; }
        const auto game_dxgi = std::filesystem::path{executable}.parent_path() / L"dxgi.dll";
        const auto real_dxgi = std::filesystem::path{system} / L"dxgi.dll";
        const auto wmr_bridge = std::filesystem::path{system} / L"MixedRealityRuntime.dll";
        std::array<HMODULE, 1024> loaded{};
        DWORD needed{};
        rejection = "loaded-module inventory unavailable or exceeds bound";
        if (!EnumProcessModules(GetCurrentProcess(), loaded.data(), sizeof(loaded), &needed) ||
            needed > sizeof(loaded) || needed % sizeof(HMODULE)) { return false; }
        for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
            const auto path = module_path(loaded[i]);
            HMODULE* destination{};
            if (detail::same_path(path, game_dxgi.native())) { destination = &proxy; }
            else if (detail::same_path(path, real_dxgi.native())) { destination = &system_dxgi; }
            else if (detail::same_path(path, wmr_bridge.native())) { destination = &bridge; }
            else if (_wcsicmp(std::filesystem::path{path}.filename().c_str(), L"WinXrRuntime.dll") == 0) {
                rejection = "unknown or excess WMR leaf runtime";
                if (!detail::wmr_leaf_path(path, program_files.data()) || leaf_count == leaves.size()) { return false; }
                destination = &leaves[leaf_count++];
            }
            if (!destination) { continue; }
            rejection = "runtime module ownership changed during preparation";
            HMODULE pinned{};
            if (*destination || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(loaded[i]), &pinned)) { return false; }
            if (!modules.keep(pinned)) { FreeLibrary(pinned); return false; }
            if (pinned != loaded[i] || !detail::same_path(module_path(pinned), path)) { return false; }
            *destination = pinned;
        }
        rejection = "required System32 DXGI, game proxy, WMR bridge, or leaf is not loaded";
        if (!proxy || !system_dxgi || !bridge || leaf_count == 0) { return false; }
        proxy_factory = reinterpret_cast<uintptr_t>(GetProcAddress(proxy, "CreateDXGIFactory1"));
        legacy_factory = reinterpret_cast<uintptr_t>(GetProcAddress(system_dxgi, "CreateDXGIFactory"));
        correct_factory = reinterpret_cast<uintptr_t>(GetProcAddress(system_dxgi, "CreateDXGIFactory1"));
        return validate();
    }
};

Repair::Repair(bool enabled, bool dx12, std::wstring_view executable, std::string_view requested_runtime) {
    if (!detail::in_scope(enabled, dx12, executable, requested_runtime)) { return; }
    try {
        auto impl = std::make_unique<Impl>();
        if (!impl->prepare(executable)) {
            spdlog::info("{} Not armed: {}; unchanged startup.", label, impl->rejection);
            return;
        }
        impl->armed = true;
        m_impl = std::move(impl);
        spdlog::info("{} Armed read-only for {} runtime import(s); ordinary xrCreateInstance runs first.", label, m_impl->leaf_count);
    } catch (const std::exception& error) {
        spdlog::warn("{} Preparation rejected: {}; unchanged startup.", label, error.what());
    }
}

Repair::~Repair() = default;

bool Repair::retry_after_failure(int32_t result, bool null_instance) {
    if (!m_impl || !m_impl->armed || !retry_gate.acquire(result, null_instance)) { return false; }
    try {
        if (!m_impl->validate()) {
            spdlog::warn("{} Correction rejected: {}; no corrected retry. baseline_result={}", label, m_impl->rejection, result);
            return false;
        }
        if (!m_impl->transaction.apply({m_impl->slots.data(), m_impl->leaf_count})) {
            spdlog::warn("{} Import transaction rejected; no corrected retry. baseline_result={}, clean={}", label, result, m_impl->transaction.clean());
            return false;
        }
        m_impl->applied = true;
        for (size_t i = 0; i < m_impl->leaf_count; ++i) {
            const auto& slot = m_impl->slots[i];
            spdlog::info("{} Runtime IAT {:x}: {:x} -> System32 CreateDXGIFactory1 {:x}; baseline_result={}",
                label, slot.address, slot.original, slot.replacement, result);
        }
        return true;
    } catch (const std::exception& error) {
        m_impl->transaction.rollback();
        spdlog::warn("{} Correction rejected: {}", label, error.what());
        return false;
    }
}

void Repair::finish(bool instance_created) {
    if (!m_impl || !m_impl->applied || m_impl->finished) { return; }
    if (instance_created) {
        m_impl->modules.retain_for_process();
        m_impl->finished = true;
        spdlog::info("{} Corrected xrCreateInstance succeeded; runtime-only route retained for this process.", label);
    } else {
        const bool clean = m_impl->transaction.rollback();
        if (!clean) { m_impl->modules.retain_for_process(); }
        m_impl->finished = true;
        spdlog::warn("{} Corrected xrCreateInstance failed; rollback={}. No further corrected retry this process.", label,
            clean ? "complete" : "incomplete; references retained, restart required");
    }
}
}
