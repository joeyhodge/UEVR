#define NOMINMAX

#include <windows.h>

#include <mutex>
#include <vector>

#include <sdk/CVar.hpp>
#include <sdk/ConsoleManager.hpp>
#include <sdk/DiscoveryMemory.hpp>
#include <sdk/FName.hpp>
#include <sdk/FSceneViewFamily.hpp>
#include <sdk/Utility.hpp>
#include <utility/Scan.hpp>
#include <utility/Module.hpp>
#include <utility/String.hpp>

#include "SatisfactoryRuntime.hpp"

namespace uevr::satisfactory {
namespace {

const std::wstring& executable_path() {
    static const auto path = utility::get_module_pathw(utility::get_executable()).value_or(L"");
    return path;
}

std::optional<Image> loaded_image(std::wstring_view component) {
    if (!is_current_runtime()) { return std::nullopt; }
    const auto& path = executable_path();
    const auto name = std::wstring{executable_prefix(path)} + L"-" + std::wstring{component} + L"-Win64-Shipping.dll";
    const auto module = GetModuleHandleW(name.c_str());
    const auto module_path = module != nullptr ? utility::get_module_pathw(module) : std::nullopt;
    if (!module_path || !owns_module(path, *module_path, component)) { return std::nullopt; }

    const auto size = GetFileVersionInfoSizeW(module_path->c_str(), nullptr);
    if (size == 0 || size > 1024 * 1024) { return std::nullopt; }
    std::vector<uint8_t> data(size);
    VS_FIXEDFILEINFO* version{};
    UINT version_size{};
    if (!GetFileVersionInfoW(module_path->c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &version_size) ||
        version_size < sizeof(*version) || version == nullptr || version->dwSignature != 0xFEEF04BD ||
        !ue561(version->dwFileVersionMS, version->dwFileVersionLS)) { return std::nullopt; }

    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto memory = sdk::discovery::process_memory();
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory.load(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        dos.e_lfanew > 0x100000 || base > (std::numeric_limits<uintptr_t>::max)() - dos.e_lfanew ||
        !memory.load(base + dos.e_lfanew, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.SizeOfImage < 0x1000 || nt.OptionalHeader.SizeOfImage > 0x20000000) { return std::nullopt; }
    return Image{base, nt.OptionalHeader.SizeOfImage};
}

std::optional<Image> cached_image(std::wstring_view component) {
    static std::mutex mutex;
    static std::array<std::optional<Image>, 3> images;
    std::scoped_lock lock{mutex};
    auto& result = images[component == L"Engine" ? 2 : component == L"Renderer" ? 1 : 0];
    if (!result) { result = loaded_image(component); }
    return result;
}

std::optional<FamilyFunctions> native_family_functions() {
    const auto engine = cached_image(L"Engine");
    if (!engine) { return {}; }
    static std::mutex mutex;
    static bool attempted{};
    static std::optional<FamilyFunctions> functions;
    std::scoped_lock lock{mutex};
    if (attempted) { return functions; }
    attempted = true;
    const auto module = reinterpret_cast<HMODULE>(engine->base);
    const auto symbol = [&](const char* name) { return reinterpret_cast<uintptr_t>(GetProcAddress(module, name)); };
    const auto copy = symbol("??0FSceneViewFamily@@QEAA@AEBV0@@Z");
    const auto destroy = symbol("??1FSceneViewFamily@@UEAA@XZ");
    const auto eye = symbol("?IsStereoEyeView@IStereoRendering@@SA_NAEBVFSceneView@@@Z");
    const auto secondary = symbol("?GetSecondaryViews@FSceneView@@QEBA?AV?$TArray@PEBVFSceneView@@V?$TSizedDefaultAllocator@$0CA@@@@@XZ");
    const auto function_size = [&](uintptr_t address) -> size_t {
        if (!engine->contains(address, 1)) { return 0; }
        const auto entry = utility::find_function_entry(address);
        return entry && engine->base + entry->BeginAddress == address && entry->EndAddress > entry->BeginAddress
            ? entry->EndAddress - entry->BeginAddress : 0;
    };
    functions = family_contract(sdk::discovery::process_memory(), *engine,
        copy, function_size(copy), destroy, function_size(destroy), eye, secondary);
    if (functions) {
        SPDLOG_INFO("[Satisfactory][NativeStereoFix] Validated Engine exports and family/view ownership contract (family=0x198, pass=0xdd0, primary=0xdd8)");
    } else {
        SPDLOG_WARN("[Satisfactory][NativeStereoFix] Engine family copy/destructor/view contract is unavailable; preserving the original render");
    }
    return functions;
}

bool independent_views(const ViewArraySnapshot& source, const ViewArraySnapshot& clone,
    const std::array<sdk::FSceneView*, 2>& entries) {
    std::array<sdk::FSceneView*, 2> copied{};
    return source.count == 2 && clone.count == 2 && source.capacity >= 2 && source.capacity <= 16 &&
        clone.capacity >= 2 && clone.capacity <= 16 && clone.data != nullptr && clone.data != source.data &&
        sdk::discovery::process_memory().load(reinterpret_cast<uintptr_t>(clone.data), copied) && copied == entries;
}

bool matches_name(const wchar_t* key, const std::wstring& name) {
    if (key == nullptr) { return false; }
    __try { return _wcsnicmp(key, name.c_str(), name.size()) == 0 && key[name.size()] == L'\0'; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

sdk::IConsoleObject* lookup(const sdk::discovery::Memory& memory, sdk::FConsoleManager* manager, const std::wstring& name) {
    static_assert(sizeof(sdk::ConsoleObjectElement) == 32);
    if (name.empty() || name.size() > 128) { return nullptr; }
    const auto array_address = reinterpret_cast<uintptr_t>(&manager->get_console_objects());
    sdk::ConsoleObjectArray before{}, after{};
    if (!memory.load(array_address, before)) { return nullptr; }
    const auto bytes = console_array_bytes(reinterpret_cast<uintptr_t>(before.elements), before.count, before.capacity);
    if (!bytes) { return nullptr; }

    // Snapshot the source-confirmed SDK array before walking it. Counts and
    // string reads are bounded; registration changes make this attempt retryable.
    std::vector<sdk::ConsoleObjectElement> entries(before.count);
    if (!memory.read(memory.context, reinterpret_cast<uintptr_t>(before.elements), entries.data(), *bytes)) { return nullptr; }
    for (const auto& entry : entries) {
        if (entry.value == nullptr || !matches_name(entry.key, name)) { continue; }
        if (!memory.load(array_address, after) || after.elements != before.elements ||
            after.count != before.count || after.capacity != before.capacity) { return nullptr; }
        return entry.value;
    }
    return nullptr;
}

} // namespace

bool is_current_runtime() {
    static const bool supported = [] {
        const auto& path = executable_path();
        if (executable_prefix(path).empty()) { return false; }
        const auto version = sdk::get_file_version_info();
        return supported_runtime(path, version.dwFileVersionMS, version.dwFileVersionLS);
    }();
    return supported;
}

std::optional<Image> renderer_image() { return cached_image(L"Renderer"); }

bool console_abi_ready() {
    return sdk::FName::s_checked_case_preserving.load(std::memory_order_acquire) && sdk::FName::runtime_size() == 8;
}

sdk::IConsoleVariable* find_console_variable(const std::wstring& name, bool floating) {
    if (!is_current_runtime() || !recover_console_variable(name) || !console_abi_ready()) { return nullptr; }
    const auto core = cached_image(L"Core");
    if (!core) { return nullptr; }
    const auto memory = sdk::discovery::process_memory();
    const auto manager = sdk::FConsoleManager::get();
    uintptr_t table{};
    if (manager == nullptr || !memory.load(reinterpret_cast<uintptr_t>(manager), table) ||
        !core->contains(table, sizeof(uintptr_t))) { return nullptr; }
    const auto object = lookup(memory, manager, name);
    const auto type = floating ? ConsoleType::Floating : equal_path(name, L"r.AllowOcclusionQueries") ? ConsoleType::Boolean : ConsoleType::Integer;
    if (!validated_console_variable(memory, *core, reinterpret_cast<uintptr_t>(object), type)) { return nullptr; }
    SPDLOG_INFO("[Satisfactory][CVar] Recovered {} through validated Core UE5.6.1 interface (Set=21, GetInt=24, GetFloat=25)",
        utility::narrow(name));
    return reinterpret_cast<sdk::IConsoleVariable*>(object);
}

sdk::FSceneViewFamily* NativeFamilyClone::get() {
    return m_constructed ? reinterpret_cast<sdk::FSceneViewFamily*>(m_storage.data()) : nullptr;
}

NativeFamilyClone::~NativeFamilyClone() {
    if (!m_constructed || !m_functions) { return; }
    // Only copied interfaces still owned by the source are detached. The native
    // destructor destroys newly installed upscalers and all engine-owned arrays.
    release_borrowed_interfaces(m_storage.data(), m_borrowed);
    reinterpret_cast<void(__fastcall*)(sdk::FSceneViewFamily*)>(m_functions->destroy)(get());
}

bool NativeFamilyClone::finish(sdk::FSceneViewFamily* source) {
    std::array<uintptr_t, 4> source_interfaces{}, clone_interfaces{};
    if (!m_constructed || !sdk::discovery::process_memory().load(
            reinterpret_cast<uintptr_t>(source) + family_interfaces_offset, source_interfaces)) { return false; }
    std::memcpy(clone_interfaces.data(), m_storage.data() + family_interfaces_offset, sizeof(clone_interfaces));
    return record_shared_interfaces(source_interfaces, clone_interfaces, m_borrowed);
}

bool NativeFamilyClone::initialize(sdk::FSceneViewFamily* source, uintptr_t expected_vtable, const char*& reason) {
    reason = "Satisfactory family capability is unavailable";
    if (m_constructed || !is_current_runtime()) { return false; }
    m_functions = native_family_functions();
    const auto engine = cached_image(L"Engine");
    if (!m_functions || !engine || !engine->contains(expected_vtable, sizeof(uintptr_t))) { return false; }
    const auto layout = sdk::FSceneViewFamily::get_layout_snapshot();
    if (layout->views != 8 || layout->render_target != 0x30 || layout->scene_interface != 0x40) {
        reason = "Satisfactory SDK family layout disagrees with the validated native copy";
        return false;
    }
    const auto memory = sdk::discovery::process_memory();
    std::array<uint8_t, family_size> source_bytes{};
    const auto address = reinterpret_cast<uintptr_t>(source);
    if (source == nullptr || !memory.load(address, source_bytes)) { reason = "source family is unreadable"; return false; }
    uintptr_t table{}, deleting_destructor{}, scene_renderer{};
    std::memcpy(&table, source_bytes.data(), sizeof(table));
    std::memcpy(&scene_renderer, source_bytes.data() + 0x158, sizeof(scene_renderer));
    std::memcpy(m_borrowed.data(), source_bytes.data() + family_interfaces_offset, sizeof(m_borrowed));
    m_original_render_safe = fresh_upscalers(m_borrowed) && scene_renderer == 0;
    if (table != expected_vtable || !memory.load(table, deleting_destructor) || !engine->contains(deleting_destructor, 1) ||
        !memory.executable(memory.context, deleting_destructor, 1) || !m_original_render_safe ||
        source_bytes[family_additional_offset] > 1) {
        reason = "source family is not a fresh Engine-owned renderer input";
        return false;
    }
    ViewArraySnapshot source_views{}, source_all{};
    std::array<sdk::FSceneView*, 2> entries{};
    std::memcpy(&source_views, source_bytes.data() + 8, sizeof(source_views));
    std::memcpy(&source_all, source_bytes.data() + 0x18, sizeof(source_all));
    if (source_views.count != 2 || source_views.capacity < 2 || source_views.capacity > 16 || source_views.data == nullptr ||
        source_all.count != 0 || source_all.capacity < 0 || source_all.capacity > 32 ||
        !memory.load(reinterpret_cast<uintptr_t>(source_views.data), entries) || entries[0] == nullptr ||
        entries[1] == nullptr || entries[0] == entries[1]) {
        reason = "source family view arrays are not a bounded two-eye input";
        return false;
    }
    using Copy = sdk::FSceneViewFamily*(__fastcall*)(sdk::FSceneViewFamily*, const sdk::FSceneViewFamily*);
    const auto result = reinterpret_cast<Copy>(m_functions->copy)(reinterpret_cast<sdk::FSceneViewFamily*>(m_storage.data()), source);
    m_constructed = true;
    ViewArraySnapshot cloned_views{}, cloned_all{};
    std::array<uintptr_t, 4> cloned_interfaces{};
    std::memcpy(&table, m_storage.data(), sizeof(table));
    std::memcpy(&cloned_views, m_storage.data() + 8, sizeof(cloned_views));
    std::memcpy(&cloned_all, m_storage.data() + 0x18, sizeof(cloned_all));
    std::memcpy(cloned_interfaces.data(), m_storage.data() + family_interfaces_offset, sizeof(cloned_interfaces));
    if (result != get() || table != m_functions->vtable || !independent_views(source_views, cloned_views, entries) ||
        cloned_all.count != 0 || cloned_all.capacity < 0 || cloned_all.capacity > 32 || cloned_interfaces != m_borrowed ||
        get()->get_scene_interface() != source->get_scene_interface() || get()->get_render_target() != source->get_render_target()) {
        reason = "native family copy failed structural/ownership validation";
        return false;
    }
    m_storage[family_additional_offset] = 1;
    reason = nullptr;
    return true;
}

} // namespace uevr::satisfactory
