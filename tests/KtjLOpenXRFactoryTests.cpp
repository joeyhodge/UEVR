#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <barrier>
#include <cstring>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include "mods/vr/KtjLOpenXRFactory.hpp"

namespace {
namespace factory = uevr::ktjl::openxr_factory;
using namespace factory::detail;
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: KTJL OpenXR factory: " << message << '\n'; }
}

struct Image {
    uintptr_t base = 0x180000000;
    std::vector<uint8_t> bytes;
    uintptr_t unreadable{};
    bool executable = true;

    Image(uint32_t size, uint32_t timestamp) : bytes(size) {
        IMAGE_DOS_HEADER dos{};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        put(0, dos);
        IMAGE_NT_HEADERS64 nt{};
        nt.Signature = IMAGE_NT_SIGNATURE;
        nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt.FileHeader.TimeDateStamp = timestamp;
        nt.FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        nt.FileHeader.NumberOfSections = 1;
        nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt.OptionalHeader.SizeOfHeaders = 0x400;
        nt.OptionalHeader.SizeOfImage = size;
        nt.OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {0x3000, 2 * sizeof(IMAGE_IMPORT_DESCRIPTOR)};
        put(0x80, nt);
    }

    template<class T> void put(uint32_t rva, const T& value) {
        std::memcpy(bytes.data() + rva, &value, sizeof(value));
    }
    void string(uint32_t rva, const char* value) { std::memcpy(bytes.data() + rva, value, std::strlen(value) + 1); }
    sdk::discovery::Memory memory() { return {this, read, code}; }
    static bool read(void* context, uintptr_t address, void* output, size_t size) {
        const auto& self = *static_cast<Image*>(context);
        if (address < self.base || address - self.base > self.bytes.size() || size > self.bytes.size() - (address - self.base) ||
            (self.unreadable >= address && self.unreadable - address < size)) { return false; }
        std::memcpy(output, self.bytes.data() + address - self.base, size);
        return true;
    }
    static bool code(void* context, uintptr_t address, size_t size) {
        const auto& self = *static_cast<Image*>(context);
        return self.executable && address >= self.base && address - self.base < self.bytes.size() && size <= self.bytes.size() - (address - self.base);
    }
};

constexpr uintptr_t proxy_factory = 0x200004976;
constexpr uintptr_t system_legacy = 0x7fff00012340;
constexpr uintptr_t system_factory = 0x7fff00022340;

Image runtime_image() {
    Image image{runtime_size, runtime_timestamp};
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    descriptor.Name = 0x5000;
    descriptor.OriginalFirstThunk = 0x4000;
    descriptor.FirstThunk = factory_iat_rva - 8;
    image.put(0x3000, descriptor);
    image.string(0x5000, "dxgi.dll");
    image.string(0x5102, "DXGIGetDebugInterface1");
    image.string(0x5202, "CreateDXGIFactory1");
    image.put(0x4000, uintptr_t{0x5100});
    image.put(0x4008, uintptr_t{0x5200});
    image.put(factory_iat_rva - 8, uintptr_t{0x7fff00044440});
    image.put(factory_iat_rva, proxy_factory);
    image.put(factory_call_rva, factory_call);
    image.put(factory_iid_rva, factory_iid);
    return image;
}

void test_scope() {
    constexpr auto game = L"D:\\Games\\KTJL\\SuicideSquad_KTJL.exe";
    expect(in_scope(true, true, game, "openxr_loader.dll"), "exact KTJL DX12 OpenXR opt-in accepted");
    expect(!in_scope(false, true, game, "openxr_loader.dll"), "default-off has no repair");
    expect(!in_scope(true, false, game, "openxr_loader.dll"), "DX11 excluded");
    expect(!in_scope(true, true, game, "openvr_api.dll"), "OpenVR excluded");
    expect(!in_scope(true, true, game, "unset"), "unrequested runtime excluded");
    expect(!in_scope(true, true, L"Other.exe", "openxr_loader.dll"), "other titles excluded");
    expect(!in_scope(true, true, L"SuicideSquad_KTJL.exe.bak", "openxr_loader.dll"), "partial filename excluded");
    expect(in_scope(true, true, L"D:/games/SUICIDESQUAD_KTJL.EXE", "openxr_loader.dll"), "case and install directory independent");

    constexpr auto root = L"C:\\Program Files";
    constexpr auto leaf = L"C:\\Program Files\\WindowsApps\\Microsoft.WindowsMixedReality.Runtime_113.2412.9001.0_x64__8wekyb3d8bbwe\\x64\\WinXrRuntime.dll";
    expect(wmr_leaf_path(leaf, root), "validated WMR package accepted");
    expect(!wmr_leaf_path(leaf, L"D:\\Program Files"), "package outside selected Program Files rejected");
    expect(!wmr_leaf_path(L"C:\\Games\\WinXrRuntime.dll", root), "runtime basename alone is insufficient");
    auto unknown = std::wstring{leaf};
    unknown.replace(unknown.find(L"113.2412"), 8, L"113.2501");
    expect(!wmr_leaf_path(unknown, root), "unvalidated WMR update rejected");
    auto preview = std::wstring{leaf};
    preview.replace(preview.find(L".Runtime_"), 9, L".PreviewRuntime_");
    expect(wmr_leaf_path(preview, root), "validated WMR preview package accepted");

    constexpr auto system = L"C:\\Windows\\System32";
    expect(wmr_manifest(R"({"file_format_version":"1.0.0","runtime":{"library_path":".\\MixedRealityRuntime.dll"}})", system), "standard WMR manifest accepted");
    expect(wmr_manifest(R"({"file_format_version":"1.0.0","runtime":{"library_path":"C:\\Windows\\System32\\MixedRealityRuntime.dll"}})", system), "absolute WMR library in System32 accepted");
    expect(!wmr_manifest(R"({"file_format_version":"1.0.0","runtime":{"library_path":"vrclient_x64.dll"}})", system), "SteamVR selection excluded even if WMR transport loaded");
    expect(!wmr_manifest(R"({"file_format_version":"1.0.0","runtime":{"library_path":"..\\MixedRealityRuntime.dll"}})", system), "out-of-System32 WMR path rejected");
    expect(!wmr_manifest(R"({"file_format_version":"1.0.0","runtime":{"library_path":"MixedRealityRuntime.dll","functions":{}}})", system), "custom negotiation exports rejected");
    expect(!wmr_manifest("{", system) && !wmr_manifest("[]", system), "malformed manifests rejected");
    expect(!wmr_manifest(std::string(16385, ' '), system), "manifest read bounded");

    // These execute the production no-op path, without loading any XR libraries.
    factory::Repair disabled{false, true, game, "openxr_loader.dll"};
    expect(!disabled.retry_after_failure(-2, true), "default-off production path cannot retry");
    factory::Repair openvr{true, true, game, "openvr_api.dll"};
    expect(!openvr.retry_after_failure(-2, true), "OpenVR production path cannot retry");
}

void test_contracts() {
    Image proxy{proxy_size, proxy_timestamp};
    proxy.base = 0x200000000;
    proxy.put(proxy_factory_rva, std::array<uint8_t, 6>{0xff, 0x25, 0xdc, 0x3f, 0x03, 0x00});
    proxy.put(proxy_forward_rva, system_legacy);
    expect(validate_proxy(proxy.memory(), proxy.base, proxy_factory, system_legacy, system_factory), "known proxy forwards Factory1 to legacy export");
    expect(!validate_proxy(proxy.memory(), proxy.base, proxy_factory + 6, system_legacy, system_factory), "other proxy exports cannot be borrowed");
    proxy.put(proxy_forward_rva, system_factory);
    expect(!validate_proxy(proxy.memory(), proxy.base, proxy_factory, system_legacy, system_factory), "already-correct forwarder is not a repair candidate");
    proxy.put(proxy_forward_rva, system_legacy);
    proxy.bytes[proxy_factory_rva + 2] ^= 1;
    expect(!validate_proxy(proxy.memory(), proxy.base, proxy_factory, system_legacy, system_factory), "unknown proxy thunk rejected");
    proxy.bytes[proxy_factory_rva + 2] ^= 1;
    proxy.executable = false;
    expect(!validate_proxy(proxy.memory(), proxy.base, proxy_factory, system_legacy, system_factory), "non-executable proxy thunk rejected");

    auto image = runtime_image();
    auto validate = [&] { return validate_runtime(image.memory(), image.base, proxy_factory, system_factory); };
    const auto slot = validate();
    expect(slot && slot->address == image.base + factory_iat_rva && slot->original == proxy_factory && slot->replacement == system_factory,
        "bounded import scan, exact callsite and Factory1 IID agree");
    image.base = 0x7ffa80000000;
    expect(validate().has_value(), "ASLR does not change the validated RVAs");
    image.put(factory_iat_rva, system_factory);
    expect(validate() && validate()->original == validate()->replacement, "already-correct runtime import is read-only");
    image.put(factory_iat_rva, uintptr_t{0x12345678});
    expect(!validate(), "third-party runtime import rejected");
    image.put(factory_iat_rva, proxy_factory);
    image.unreadable = image.base + factory_iat_rva;
    expect(!validate(), "unreadable IAT rejected");
    image.unreadable = 0;
    image.executable = false;
    expect(!validate(), "non-executable callsite rejected");
    image.executable = true;
    image.bytes[factory_call_rva] ^= 1;
    expect(!validate(), "changed callsite rejected");
    image.bytes[factory_call_rva] ^= 1;
    image.bytes[factory_iid_rva] ^= 1;
    expect(!validate(), "different factory IID rejected");
    image.bytes[factory_iid_rva] ^= 1;
    image.put(0x88, uint32_t{runtime_timestamp + 1});
    expect(!validate(), "unknown runtime build rejected");
    image.put(0x88, runtime_timestamp);
    image.put(0x3c, uint32_t{0xfffffff0});
    expect(!validate(), "negative PE offset rejected before addressing");
    image.put(0x3c, uint32_t{0x80});
    image.put(0x4008, uintptr_t{runtime_size - 1});
    expect(!validate(), "overflowing import-by-name rejected");
    image.put(0x4008, uintptr_t{0x5200});
    image.put(0x4000, uintptr_t{0x5200});
    expect(!validate(), "duplicate Factory1 imports rejected");
    image.put(0x4000, uintptr_t{IMAGE_ORDINAL_FLAG64 | 1});
    expect(validate().has_value(), "unrelated ordinal import safely skipped");
    image.put(0x4000, uintptr_t{0x5100});
    image.put(0x4010, uintptr_t{0x5300});
    image.string(0x5302, "Other");
    std::fill(image.bytes.begin() + 0x5302, image.bytes.begin() + 0x5302 + 128, 'X');
    expect(!validate(), "unterminated import name is bounded");
    image.put(0x4010, uintptr_t{0});
    image.put(factory_iat_rva + 8, system_factory);
    expect(!validate(), "IAT terminator must agree with name table");
    image.put(factory_iat_rva + 8, uintptr_t{0});
    image.put(0x3010, uint32_t{factory_iat_rva});
    expect(!validate(), "wrong IAT layout rejected even with otherwise-valid names");
    image.put(0x3010, uint32_t{factory_iat_rva - 8});
    auto header = *reinterpret_cast<IMAGE_NT_HEADERS64*>(image.bytes.data() + 0x80);
    header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = sizeof(IMAGE_IMPORT_DESCRIPTOR);
    image.put(0x80, header);
    expect(!validate(), "missing descriptor terminator rejected");
    header.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = {runtime_size - 4, 40};
    image.put(0x80, header);
    expect(!validate(), "import directory outside image rejected");
}

struct Editor {
    std::array<Slot, 2> slots{{{0x100008, 0x200000, 0x300000}, {0x101008, 0x200100, 0x300000}}};
    std::array<uintptr_t, 2> values{0x200000, 0x200100};
    std::array<uint32_t, 2> protection{PAGE_READONLY, PAGE_READONLY};
    std::set<int> fail_protect, fail_exchange;
    int protects{}, exchanges{}, race_exchange{};
    size_t index(uintptr_t address) const { return address == slots[0].address ? 0 : 1; }
    EditApi api() { return {this, protect, exchange}; }
    static bool protect(void* context, uintptr_t address, uint32_t desired, uint32_t& old) {
        auto& self = *static_cast<Editor*>(context);
        if (self.fail_protect.contains(++self.protects)) { return false; }
        const auto index = self.index(address);
        old = self.protection[index];
        for (size_t i = 0; i < self.slots.size(); ++i) {
            if ((self.slots[i].address & ~uintptr_t{0xfff}) == (address & ~uintptr_t{0xfff})) { self.protection[i] = desired; }
        }
        return true;
    }
    static bool exchange(void* context, uintptr_t address, uintptr_t expected, uintptr_t desired, uintptr_t& observed) {
        auto& self = *static_cast<Editor*>(context);
        ++self.exchanges;
        if (self.fail_exchange.contains(self.exchanges)) { return false; }
        auto& value = self.values[self.index(address)];
        if (self.exchanges == self.race_exchange) { value = 0x444400; }
        observed = value;
        if (value == expected) { value = desired; }
        return true;
    }
    bool restored() const { return values[0] == slots[0].original && values[1] == slots[1].original && protection[0] == PAGE_READONLY && protection[1] == PAGE_READONLY; }
};

void test_edits() {
    {
        Editor editor;
        Transaction tx{editor.api()};
        expect(tx.apply(editor.slots) && !tx.clean(), "two-slot transaction applied");
        expect(editor.values[0] == 0x300000 && editor.values[1] == 0x300000 && editor.protection[0] == PAGE_READONLY && editor.protection[1] == PAGE_READONLY, "pointers corrected, original page protections restored");
        expect(tx.rollback() && editor.restored(), "failure rolls back both slots exactly");
        expect(!tx.apply(editor.slots), "same transaction cannot be replayed");
    }
    {
        Editor editor;
        editor.slots[1].address = editor.slots[0].address + 8;
        Transaction tx{editor.api()};
        expect(tx.apply(editor.slots) && tx.rollback() && editor.restored(), "two imports on one page restore exact protection");
    }
    for (int failed_call = 1; failed_call <= 4; ++failed_call) {
        Editor editor;
        editor.fail_protect.insert(failed_call);
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && tx.clean() && editor.restored(), "protection failure unwinds partial batch");
    }
    for (int failed_call = 1; failed_call <= 2; ++failed_call) {
        Editor editor;
        editor.fail_exchange.insert(failed_call);
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && tx.clean() && editor.restored(), "exchange failure unwinds partial batch");
    }
    {
        Editor editor;
        editor.race_exchange = 2;
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && tx.clean() && editor.values[0] == editor.slots[0].original && editor.values[1] == 0x444400,
            "second-slot ownership race restores first but preserves other writer");
    }
    {
        Editor editor;
        Transaction tx{editor.api()};
        expect(tx.apply(editor.slots), "prepare external-writer rollback test");
        editor.values[0] = 0x444400;
        expect(tx.rollback() && editor.values[0] == 0x444400 && editor.values[1] == editor.slots[1].original, "rollback never overwrites a subsequent owner's pointer");
    }
    {
        Editor editor;
        editor.fail_protect = {2, 4};
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && !tx.clean(), "failed protection restoration remains explicitly pending");
        expect(tx.rollback() && editor.restored(), "pending original protection survives a cleanup retry");
    }
    {
        Editor editor;
        editor.fail_protect = {2, 3, 4};
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && !tx.rollback() && !tx.clean(), "unrestored pointer cannot be mistaken for safe module release");
        expect(tx.rollback() && editor.restored(), "ownership retained until actual cleanup succeeds");
    }
    {
        Editor editor;
        editor.slots[1].address = 0;
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && editor.protects == 0 && editor.exchanges == 0, "all slots validated before any mutation");
    }
    {
        Editor editor;
        editor.slots[1].address = editor.slots[0].address;
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && editor.protects == 0, "duplicate slots rejected before mutation");
    }
    {
        Editor editor;
        for (auto& slot : editor.slots) { slot.original = slot.replacement; }
        Transaction tx{editor.api()};
        expect(!tx.apply(editor.slots) && tx.clean() && editor.protects == 0, "already-correct imports never made writable");
    }
}

void test_retry() {
    RetryGate gate;
    expect(!gate.acquire(0, true) && !gate.acquire(-6, true) && !gate.acquire(-2, false), "only the exact null-instance runtime failure is eligible");
    std::barrier start{8};
    std::atomic<int> granted{};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] { start.arrive_and_wait(); if (gate.acquire(-2, true)) { ++granted; } });
    }
    for (auto& thread : threads) { thread.join(); }
    expect(granted == 1 && !gate.acquire(-2, true), "one corrected retry per process, including concurrent requests");
}
}

int test_ktjl_openxr_factory() {
    failures = 0;
    test_scope();
    test_contracts();
    test_edits();
    test_retry();
    return failures;
}
