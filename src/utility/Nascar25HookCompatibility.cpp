#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <safetyhook/os.hpp>
#include <spdlog/spdlog.h>
#include <utility/ScopeGuard.hpp>
#include "NascarHookCompatibility.hpp"
#include "Logging.hpp"

namespace uevr::nascar::title25 {
namespace {
std::once_flag g_once;
std::atomic_bool g_target{}, g_validated{};
uintptr_t g_image{};
size_t g_size{};

bool preserve_image(uint8_t* address, size_t bytes, uint32_t protection, uint32_t* old) {
    if (intersects_image(reinterpret_cast<uintptr_t>(address), bytes, g_image, g_size)) {
        SPDLOG_ERROR_ONCE("[NASCAR25][CodePreserving] Refused a game-image write; no integrity-checker or instruction patches");
    }
    return protect_external_memory(address, bytes, protection, old, g_image, g_size);
}

bool known_file(const wchar_t* filename) {
    constexpr std::array<uint8_t, 32> expected{
        0x45,0x36,0x72,0xf7,0x89,0xad,0xe1,0x39,0xd5,0x45,0x2b,0x27,0xa4,0x27,0x51,0xa9,
        0xd8,0xf3,0xb1,0x2b,0x10,0xfb,0x8a,0xaf,0x3b,0xb5,0xbd,0x7b,0x66,0xda,0x1a,0x26};
    std::ifstream input{std::filesystem::path{filename}, std::ios::binary};
    if (!input) { return false; }
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) { return false; }
    utility::ScopeGuard close_algorithm{[&] { BCryptCloseAlgorithmProvider(algorithm, 0); }};
    BCRYPT_HASH_HANDLE hash{};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) { return false; }
    utility::ScopeGuard close_hash{[&] { BCryptDestroyHash(hash); }};
    std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(input.gcount()), 0) < 0) { return false; }
    }
    if (!input.eof()) { return false; }
    std::array<uint8_t, 32> actual{};
    return BCryptFinishHash(hash, actual.data(), static_cast<ULONG>(actual.size()), 0) >= 0 && actual == expected;
}

bool code(uintptr_t rva, std::initializer_list<uint8_t> expected) {
    if (!g_image || !rva || expected.size() > 128 || rva >= g_size || expected.size() > g_size - rva) { return false; }
    std::array<uint8_t, 128> actual{};
    MEMORY_BASIC_INFORMATION page{};
    constexpr DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(reinterpret_cast<void*>(g_image + rva), &page, sizeof(page)) == sizeof(page) &&
        page.State == MEM_COMMIT && page.Type == MEM_IMAGE && reinterpret_cast<uintptr_t>(page.AllocationBase) == g_image &&
        (page.Protect & execute) && !(page.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
        read_memory(g_image + rva, actual.data(), expected.size()) &&
        std::equal(expected.begin(), expected.end(), actual.begin());
}

bool slot(uintptr_t table, size_t count, size_t index, uintptr_t function) {
    return find_virtual_slot_in_image(g_image + table, g_image + function, count, g_image) == g_image + table + index * sizeof(uintptr_t);
}

// Exact, independently recovered NASCAR25 instruction contracts. These are
// comparisons only; no bytes in the game image are changed.
#include "Nascar25HookSignatures.inl"
}

void initialize() {
    std::call_once(g_once, [] {
        std::array<wchar_t, 32768> path{};
        const auto n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!n || n >= path.size() || !matches_name({path.data(), n})) { return; }
        g_target.store(true, std::memory_order_release);
        g_image = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};
        IMAGE_NT_HEADERS64 nt{};
        if (!read_memory(g_image, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 || !read_memory(g_image + dos.e_lfanew, &nt, sizeof(nt)) ||
            nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            !nt.OptionalHeader.SizeOfImage || nt.OptionalHeader.SizeOfImage > 0x40000000) { return; }
        g_size = nt.OptionalHeader.SizeOfImage;
        if (safetyhook::has_protection_override()) {
            SPDLOG_ERROR("[NASCAR25][CodePreserving] Existing protection backend; adapters refused");
            return;
        }
        safetyhook::set_protection_override(preserve_image);
        DWORD ignored{};
        const auto bytes = GetFileVersionInfoSizeW(path.data(), &ignored);
        if (!bytes || bytes > 1024 * 1024) { return; }
        std::vector<uint8_t> version(bytes);
        VS_FIXEDFILEINFO* info{};
        UINT size{};
        if (!GetFileVersionInfoW(path.data(), 0, bytes, version.data()) ||
            !VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&info), &size) ||
            !info || size < sizeof(*info) || info->dwSignature != 0xfeef04bd ||
            !matches_build_metadata(info->dwFileVersionMS, info->dwFileVersionLS, nt.FileHeader.TimeDateStamp, g_size) ||
            !known_file(path.data()) || !base_contract()) {
            SPDLOG_ERROR("[NASCAR25][CodePreserving] Unvalidated game image/ABI; no game-code hooks or object adapters enabled");
            return;
        }
        g_validated.store(true, std::memory_order_release);
        SPDLOG_INFO("[NASCAR25][CodePreserving] Validated UE5.5.4 image, engine/viewport/Slate dispatch and texture chain; image patches refused");
    });
}

bool is_target() { return g_target.load(std::memory_order_acquire); }
bool is_validated_build() { return g_validated.load(std::memory_order_acquire); }
uintptr_t image_base() { return g_image; }
bool validate_rhi_command_layout() { return is_validated_build() && rhi_command_contract(); }
bool validate_synced_redraw() { return is_validated_build() && synced_contract(); }
bool validate_ghost_view_setup() {
    return is_validated_build() && ghost_contract() &&
        slot(layout25.localplayer_vtable_rva, layout25.localplayer_slots, layout25.init_options_slot, layout25.init_options_rva) &&
        slot(layout25.localplayer_vtable_rva, layout25.localplayer_slots, layout25.calc_scene_view_slot, layout25.calc_scene_view_rva) &&
        slot(layout25.localplayer_vtable_rva, layout25.localplayer_slots, layout25.projection_data_slot, layout25.projection_data_rva);
}
bool validate_native_rooting() { return is_validated_build() && root_contract(); }
bool validate_native_renderer() {
    return validate_ghost_view_setup() && validate_native_rooting() && native_contract() && gamma_contract() &&
        slot(layout25.renderer_vtable_rva, layout25.renderer_slots, 8, layout25.renderer_single_rva) &&
        slot(layout25.viewport_vtable_rva, 16, 6, 0x3ed5410) &&
        slot(layout25.native_capture_frt_vtable_rva, 16, 6, layout25.native_capture_display_gamma_rva);
}

bool native_owned_object_valid(const NativeOwnedObject& owner, uintptr_t item, bool require_root) {
    struct Item { uintptr_t object; uint32_t flags; int32_t cluster, serial, refcount; } before{}, after{};
    struct Header { uintptr_t vtable; uint32_t flags; int32_t index; } header{};
    const auto matches = [&](const Item& observed) {
        constexpr uint32_t invalid = (1u << 21) | (1u << 28) | (1u << 31);
        return observed.object == owner.object && !(observed.flags & invalid) &&
            (!require_root || (observed.flags & native_owner_root_flag));
    };
    return owner.object && owner.vtable && owner.index >= 0 && item &&
        read_memory(item, &before, sizeof(before)) && matches(before) &&
        read_memory(owner.object, &header, sizeof(header)) && header.vtable == owner.vtable && header.index == owner.index &&
        read_memory(item, &after, sizeof(after)) && matches(after) && after.serial == before.serial;
}

std::optional<float> read_native_display_gamma() {
    if (!is_validated_build()) { return {}; }
    uintptr_t engine{}, client{}, viewport{}, table{}, engine2{}, client2{}, viewport2{};
    float engine_gamma{}, viewport_gamma{};
    uint8_t override_enabled{};
    if (!read_memory(g_image + layout25.engine_global_rva, &engine, sizeof(engine)) || !engine ||
        !read_memory(engine + layout25.engine_viewport_offset, &client, sizeof(client)) || !client ||
        !read_memory(client + 0xf8, &viewport, sizeof(viewport)) || !viewport ||
        !read_memory(viewport, &table, sizeof(table)) || table != g_image + layout25.viewport_vtable_rva ||
        !read_memory(viewport + 0x300, &override_enabled, sizeof(override_enabled)) ||
        !read_memory(viewport + 0x2fc, &viewport_gamma, sizeof(viewport_gamma)) ||
        (!override_enabled && !read_memory(engine + 0xd64, &engine_gamma, sizeof(engine_gamma))) ||
        !read_memory(g_image + layout25.engine_global_rva, &engine2, sizeof(engine2)) || engine2 != engine ||
        !read_memory(engine + layout25.engine_viewport_offset, &client2, sizeof(client2)) || client2 != client ||
        !read_memory(client + 0xf8, &viewport2, sizeof(viewport2)) || viewport2 != viewport) { return {}; }
    return select_native_display_gamma(engine_gamma, viewport_gamma, override_enabled);
}
}
