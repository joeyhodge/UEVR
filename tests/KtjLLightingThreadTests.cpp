#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include "mods/vr/KtjLLightingThread.hpp"

namespace {
namespace l = uevr::ktjl::lighting;
namespace f = uevr::ktjl::fog;
namespace k = sdk::ktjl;
constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
constexpr uintptr_t state = 0x62000000, stack = 0x64000000;
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

struct Memory {
    struct Block {
        uintptr_t address{};
        std::vector<uint8_t> bytes{};
        bool executable{};
        bool operator==(const Block&) const = default;
    };
    std::vector<Block> blocks{};
    uintptr_t change_address{};
    size_t change_read{}, reads{};
    void add(uintptr_t a, const void* p, size_t n, bool executable = false) {
        const auto bytes = static_cast<const uint8_t*>(p);
        blocks.push_back({a, {bytes, bytes + n}, executable});
    }
    template<class T> void put(uintptr_t a, const T& value) {
        for (auto& b : blocks) {
            if (a >= b.address && a - b.address <= b.bytes.size() && sizeof(T) <= b.bytes.size() - (a - b.address)) {
                std::memcpy(b.bytes.data() + a - b.address, &value, sizeof(value)); return;
            }
        }
        add(a, &value, sizeof(value));
    }
    sdk::discovery::Memory reader() {
        return {this, [](void* context, uintptr_t a, void* p, size_t n) {
            auto& self = *static_cast<Memory*>(context);
            for (const auto& b : self.blocks) {
                if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                    std::memcpy(p, b.bytes.data() + a - b.address, n);
                    if (a == self.change_address && ++self.reads == self.change_read) {
                        static_cast<uint8_t*>(p)[0] ^= 0x08;
                    }
                    return true;
                }
            }
            return false;
        }, [](void* context, uintptr_t a, size_t n) {
            for (const auto& b : static_cast<Memory*>(context)->blocks) {
                if (b.executable && a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) { return true; }
            }
            return false;
        }};
    }
};

Memory pair_fixture() {
    Memory m;
    m.put(renderer + f::views_offset, f::Header{views, 2, 2});
    for (size_t i = 0; i != 2; ++i) {
        m.put(views + i * f::view_stride, f::ViewPrefix{base + f::view_vtable_rva, 0, renderer + 0x10, state + i * 0x10000});
        m.put(views + i * f::view_stride + uevr::ktjl::cloud::state_offset, state + i * 0x10000);
        m.put(state + i * 0x10000, base + k::stereo::state_vtable_rva);
    }
    m.put(stack + l::return_stack_offset, base + l::caller_return_rva);
    return m;
}

safetyhook::Context context_fixture() {
    safetyhook::Context c{};
    c.rax = 0x123456789ABC0001;
    c.rdi = renderer; c.rsi = renderer + f::views_offset;
    c.r13 = base + l::immediate_rva; c.rsp = stack;
    c.rcx = 0xABC; c.rdx = 0xDEF; c.rip = base + l::boundary_rva;
    return c;
}

void test_selection() {
    auto m = pair_fixture();
    auto c = context_fixture();
    const auto before = m.blocks;
    expect(l::select_serial(m.reader(), base, c), "the captured two-view immediate-list caller selects serial lighting");
    auto expected = c;
    expected.rax &= ~uintptr_t{0xFF};
    l::use_serial(c);
    expect(std::memcmp(&c, &expected, sizeof(c)) == 0, "only AL changes; flags, stack, arguments and SIMD state stay intact");
    expect(m.blocks == before, "selection does not write a task, fence, view, RHI list, or CVar");
    expect(!l::select_serial(m.reader(), base, c), "an existing serial decision is not changed");
    for (int mode = 0; mode != 21; ++mode) {
        auto invalid = pair_fixture();
        c = context_fixture();
        if (mode == 0) { c.rax = 2; }
        if (mode == 1) { c.rdi = 0; }
        if (mode == 2) { c.rdi = ~uintptr_t{}; }
        if (mode == 3) { c.rsi += 8; }
        if (mode == 4) { c.r13 += 8; }
        if (mode == 5) { c.rsp = 1; }
        if (mode == 6) { c.rsp = 0x00007FFFFFFFFFF8; }
        if (mode == 7) { invalid.put(stack + l::return_stack_offset, base + l::caller_return_rva + 1); }
        if (mode == 8) { invalid.put(renderer + f::views_offset, f::Header{views, 1, 2}); }
        if (mode == 9) { invalid.put(renderer + f::views_offset, f::Header{views, 3, 3}); }
        if (mode == 10) { invalid.put(renderer + f::views_offset, f::Header{views, 2, 1}); }
        if (mode == 11) { invalid.put(renderer + f::views_offset, f::Header{views, 2, 17}); }
        if (mode == 12) { invalid.put(renderer + f::views_offset, f::Header{1, 2, 2}); }
        if (mode == 13) { invalid.put(views, base + f::view_vtable_rva + 8); }
        if (mode == 14) { invalid.put(state + 0x10000, base + k::stereo::state_vtable_rva + 8); }
        if (mode == 15) { invalid.put(views + f::view_stride + 0x10, renderer + 0x20); }
        if (mode == 16) { invalid.put(views + f::view_stride + uevr::ktjl::cloud::state_offset, state); }
        if (mode == 17) {
            invalid.put(views + f::view_stride + 0x18, state);
            invalid.put(views + f::view_stride + uevr::ktjl::cloud::state_offset, state);
        }
        if (mode == 18) { invalid.blocks.pop_back(); }
        if (mode >= 19) {
            invalid.change_address = renderer + f::views_offset;
            invalid.change_read = mode == 19 ? 2 : 3;
        }
        expect(!l::select_serial(invalid.reader(), base, c), "unrelated, mono, aliased, unreadable or changing contexts retain engine scheduling");
    }
    // Model the original store/launch and later join: no detached task, no
    // duplicate draw and no wait are introduced by a pre-launch false decision.
    size_t workers{}, serial{}, waits{};
    for (size_t frame = 0; frame != 64; ++frame) {
        c = context_fixture();
        if (l::select_serial(m.reader(), base, c)) { l::use_serial(c); }
        const bool queued = (c.rax & 0xFF) != 0;
        if (queued) { ++workers; }
        if (queued) { ++waits; } else { ++serial; }
    }
    expect(workers == 0 && waits == 0 && serial == 64, "each stereo frame runs the existing serial pass once without launching or joining a worker");
}

Memory contract_fixture() {
    Memory m;
    std::array<uint8_t, 512> pe{};
    m.add(base, pe.data(), pe.size());
    m.put(base, uint16_t{0x5A4D}); m.put(base + 0x3C, uint32_t{0xC0});
    m.put(base + 0xC0, uint32_t{0x4550}); m.put(base + 0xC4, uint16_t{0x8664});
    m.put(base + 0xC8, k::image_timestamp); m.put(base + 0xD8, uint16_t{0x20B}); m.put(base + 0x110, k::image_size);
    m.add(base + 0xAFFF76, k::class_name_access.data(), k::class_name_access.size(), true);
    m.add(base + 0xA1DC2F, k::object_iteration.data(), k::object_iteration.size(), true);
    m.add(base + 0x58DC4E, k::name_entry_access.data(), k::name_entry_access.size(), true);
    for (const auto& e : l::code_evidence) { m.add(base + e.rva, e.bytes.data(), e.bytes.size(), true); }
    return m;
}

void test_contracts(const wchar_t* path) {
    expect(f::supports(L"D:\\Games\\SuicideSquad_KTJL.exe", true) &&
        !f::supports(L"SuicideSquad_KTJL.exe", false) && !f::supports(L"Other.exe", true), "lighting guard is KTJL DX12 only");
    auto m = contract_fixture();
    expect(l::validate_code(m.reader(), base), "scheduler and its existing serial continuation validate offline");
    for (const auto& e : l::code_evidence) {
        for (size_t i = 0; i != e.bytes.size(); ++i) {
            m.put(base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!l::validate_code(m.reader(), base), "every changed instruction-contract byte prevents installation");
            m.put(base + e.rva + i, e.bytes[i]);
        }
        for (bool truncate : {false, true}) {
            auto invalid = m;
            for (auto& b : invalid.blocks) {
                if (b.address == base + e.rva) { if (truncate) { b.bytes.pop_back(); } else { b.executable = false; } }
            }
            expect(!l::validate_code(invalid.reader(), base), "truncated or non-executable code rejects the guard");
        }
    }
    m.put(base + 0xC8, uint32_t{k::image_timestamp + 1});
    expect(!l::validate_code(m.reader(), base), "unknown game builds cannot inherit the fixed scheduler layout");
    if (!path) { return; }
    std::ifstream file(std::filesystem::path{path}, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    Memory captured;
    captured.add(base, bytes.data(), bytes.size(), true);
    expect(file.is_open() && l::validate_code(captured.reader(), base), "all scheduler and serial-work contracts match the captured KTJL runtime image");
}

bool serial_requested{};
void (*clobber)(){};
void callback(safetyhook::Context& context) {
    clobber();
    if (serial_requested && (context.rax & 0xFF) == 1) { l::use_serial(context); }
}

void test_native_hook(bool far_placement) {
    const size_t reserve_size = far_placement ? 0x110000000ull : 0x400000;
    auto* reserved = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reserved != nullptr, "lighting test reserves address space");
    if (!reserved) { return; }
    auto* page = reserved + (far_placement ? 0x88000000ull : 0x200000);
    if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
        expect(false, "lighting fixture page commits"); VirtualFree(reserved, 0, MEM_RELEASE); return;
    }
    std::memset(page, 0x90, 0x1000);
    auto* entry = page + 0x100;
    auto* boundary = page + 0x200;
    // Preserve the real MOV [RDI+941h],AL / MOV RAX,[RSP+B0h] span,
    // including its 14-byte far-trampoline form and live volatile/SIMD inputs.
    const uint8_t prefix[]{0x57,0x48,0x81,0xEC,0xC0,0,0,0,0x48,0x8B,0xF9,0x48,0x8B,0xC2,
        0x49,0xBA,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
        0x4C,0x89,0x94,0x24,0xB0,0,0,0,0xF3,0x41,0x0F,0x6F,0x29};
    std::memcpy(entry, prefix, sizeof(prefix));
    std::memcpy(boundary, l::boundary_code.data(), l::boundary_code.size());
    const uint8_t tail[]{0x49,0x89,0x00,0x49,0x89,0x50,0x08,0x49,0x89,0x48,0x10,
        0x4D,0x89,0x48,0x18,0xF3,0x41,0x0F,0x7F,0x68,0x20,
        0x0F,0xB6,0x87,0x41,0x09,0,0,0x48,0x81,0xC4,0xC0,0,0,0,0x5F,0xC3};
    std::memcpy(boundary + l::boundary_code.size(), tail, sizeof(tail));
    const uint8_t clobber_code[]{0x31,0xC9,0x31,0xD2,0x45,0x31,0xC0,0x45,0x31,0xC9,
        0x45,0x31,0xD2,0x45,0x31,0xDB,0x66,0x0F,0xEF,0xED,0xC3};
    std::memcpy(page + 0x800, clobber_code, sizeof(clobber_code));
    clobber = reinterpret_cast<void(*)()>(page + 0x800);
    DWORD old{};
    expect(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old), "lighting fixture is executable/read-only");
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    using Call = uint64_t(*)(uint8_t*, uint64_t, uint64_t*, const uint64_t*);
    const auto call = reinterpret_cast<Call>(entry);
    std::array<uint8_t, 0x980> object{};
    std::array<uint64_t, 6> out{};
    std::array<uint64_t, 2> vector{0xABCDEF0123456789,0x123456789ABCDEF0};
    expect(call(object.data(), 1, out.data(), vector.data()) == 1, "unmodified boundary retains the parallel decision");
    {
        const auto synthetic_base = reinterpret_cast<uintptr_t>(boundary) - l::boundary_rva;
        auto hook = l::prepare_hook(synthetic_base, callback,
            [](void* at, safetyhook::MidHookFn fn, safetyhook::MidHook::Flags flags) {
                return safetyhook::MidHook::create(safetyhook::Allocator::create(), at, fn, flags);
            });
        expect(hook.has_value(), "exact lighting boundary prepares with near or far placement");
        if (hook) {
            if (far_placement) { expect(hook->original_bytes().size() >= 14, "far placement really relocates both store and stack load"); }
            expect(call(object.data(), 1, out.data(), vector.data()) == 1, "prepared hook remains disabled until activation");
            expect(hook->enable().has_value(), "lighting guard enables");
            for (const auto requested : {false, true}) {
                serial_requested = requested;
                for (const uint64_t original : {0ull, 1ull, 2ull}) {
                    out = {};
                    const auto result = call(object.data(), original, out.data(), vector.data());
                    expect(result == ((requested && original == 1) ? 0 : original), "engine store receives only the intended boolean override");
                    expect(out[0] == 0x1122334455667788 && out[1] == original &&
                        out[2] == reinterpret_cast<uintptr_t>(object.data()) && out[3] == reinterpret_cast<uintptr_t>(vector.data()) &&
                        out[4] == vector[0] && out[5] == vector[1], "callback cannot clobber live arguments, vector data or the original stack load");
                }
            }
            expect(hook->disable().has_value() && call(object.data(), 1, out.data(), vector.data()) == 1,
                "uninstall restores original scheduling and machine code");
        }
    }
    VirtualFree(reserved, 0, MEM_RELEASE);
}
}

int test_ktjl_lighting_thread(const wchar_t* path) {
    test_selection();
    test_contracts(path);
    test_native_hook(false);
    test_native_hook(true);
    std::cout << "KTJL lighting thread ownership: " << failures << " failures\n";
    return failures;
}
