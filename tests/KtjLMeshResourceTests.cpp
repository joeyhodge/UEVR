#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <intrin.h>
#include <thread>
#include <vector>
#include <sdk/DiscoveryMemory.hpp>
#include "mods/vr/KtjLMeshResourceHook.hpp"

namespace {
namespace m = uevr::ktjl::mesh;
namespace f = uevr::ktjl::fog;
namespace k = sdk::ktjl;
constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
constexpr uintptr_t state = 0x62000000, stack = 0x64000000, collector = renderer + m::collector_offset;
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
            for (const auto& b : static_cast<Memory*>(context)->blocks) {
                if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                    std::memcpy(p, b.bytes.data() + a - b.address, n); return true;
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

safetyhook::Context context_fixture() {
    safetyhook::Context c{};
    c.rsp = stack - 8; c.rbp = stack + m::frame_pointer_offset;
    c.rdx = c.rbp + 0x28; c.r12 = collector; c.rsi = views;
    c.r8 = 0x123456789ABC0004; c.rcx = 50;
    return c;
}

Memory pair_fixture() {
    Memory mem;
    mem.put(renderer + f::views_offset, f::Header{views, 2, 2});
    for (size_t i = 0; i != 2; ++i) {
        mem.put(views + i * f::view_stride, f::ViewPrefix{base + f::view_vtable_rva, 0, renderer + 0x10, state + i * 0x10000});
        mem.put(views + i * f::view_stride + uevr::ktjl::cloud::state_offset, state + i * 0x10000);
        mem.put(state + i * 0x10000, base + k::stereo::state_vtable_rva);
    }
    const auto c = context_fixture();
    mem.put(c.rsp, base + 0x75EF57);
    mem.put(stack + m::caller_offset, base + m::callers[0]);
    mem.put(c.rdx, std::array<uintptr_t, 2>{base + m::work_rva, c.rbp - 0x28});
    mem.put(collector + 0x2DC, uint32_t{7});
    mem.put(collector + 0x2E0, uint8_t{1});
    return mem;
}

void test_selection() {
    auto mem = pair_fixture();
    auto c = context_fixture();
    const auto bytes_before = mem.blocks;
    for (auto caller : m::callers) {
        mem.put(stack + m::caller_offset, base + caller);
        expect(m::select_serial(mem.reader(), base, c), "all three validated per-view gather callers select serial work");
    }
    mem.put(stack + m::caller_offset, base + m::callers[0]);
    auto expected = c; expected.r8 |= 1;
    m::use_serial(c);
    expect(std::memcmp(&c, &expected, sizeof(c)) == 0, "only ForceSingleThread changes; other flags, registers, stack and SIMD are preserved");
    expect(mem.blocks == bytes_before, "selection never rewrites collector memory or a global CVar");
    c.rsi += f::view_stride;
    expect(m::select_serial(mem.reader(), base, c), "the second view uses the same lifetime repair as the first");
    for (int n = 0; n != 20; ++n) {
        auto invalid = pair_fixture(); c = context_fixture();
        if (n == 0) { c.rbp += 8; }
        if (n == 1) { c.rsp = ~uintptr_t{}; }
        if (n == 2) { c.rdx += 8; }
        if (n == 3) { c.r12 += 0x330; }
        if (n == 4) { c.rsi += 8; }
        if (n == 5) { invalid.put(c.rsp, base + 0x75EF58); }
        if (n == 6) { invalid.put(stack + m::caller_offset, base + m::callers[0] + 1); }
        if (n == 7) { invalid.put(c.rdx, base + m::work_rva + 1); }
        if (n == 8) { invalid.put(c.rdx + 8, c.rbp - 0x20); }
        if (n == 9) { invalid.put(renderer + f::views_offset, f::Header{views, 1, 2}); }
        if (n == 10) { invalid.put(renderer + f::views_offset, f::Header{views, 3, 3}); }
        if (n == 11) { invalid.put(renderer + f::views_offset, f::Header{views, 2, 1}); }
        if (n == 12) { invalid.put(views, base + f::view_vtable_rva + 8); }
        if (n == 13) { invalid.put(views + f::view_stride + 0x10, renderer + 0x20); }
        if (n == 14) { invalid.put(state + 0x10000, base + k::stereo::state_vtable_rva + 8); }
        if (n == 15) {
            invalid.put(views + f::view_stride + 0x18, state);
            invalid.put(views + f::view_stride + uevr::ktjl::cloud::state_offset, state);
        }
        if (n == 16) { invalid.put(collector + 0x2DC, UINT32_MAX); }
        if (n == 17) { invalid.put(collector + 0x2E0, uint8_t{0}); }
        if (n == 18) { invalid.blocks.pop_back(); }
        if (n == 19) { c.rsp = 1; }
        expect(!m::select_serial(invalid.reader(), base, c), "unrelated, mono, stale, aliased or unreadable contexts retain their original path");
    }
    for (bool target : {false, true}) for (bool native : {false, true}) for (bool fix : {false, true}) {
        expect(m::keep_native_fallback(target, native, fix) == (target && native && !fix),
            "ordinary KTJL Native preserves mono; requested Native Fix, Synced and other games are not forced mono");
    }
}

thread_local m::GatherScope<2> thread_scope{};
void test_scope() {
    auto mem = pair_fixture();
    safetyhook::Context a{};
    a.rsp = stack - 0x500; a.rbx = collector; a.r8 = 0x65000000;
    m::GatherScope<2> scope;
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "no active gather means no allocation override");
    scope.begin(stack, collector);
    expect(scope.use_frame_allocator(mem.reader(), base, a), "validated serial gather redirects only its collector");
    a.rbx += 0x330;
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "the independent shadow collector is untouched");
    a.rbx = collector;
    scope.begin(stack - 0x100, 0);
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "an unrelated nested gather cannot inherit the outer repair");
    scope.begin(stack - 0x200, collector);
    expect(scope.use_frame_allocator(mem.reader(), base, a), "bounded TLS overflow revalidates the collector rather than dropping the repair");
    mem.put(collector + 0x2E0, uint8_t{0});
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "overflow cannot redirect an inactive collector");
    mem.put(collector + 0x2E0, uint8_t{1});
    scope.end(stack - 0x200);
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "leaving overflow restores the unrelated nested scope");
    scope.end(stack - 0x100);
    expect(scope.use_frame_allocator(mem.reader(), base, a), "leaving the nested scope restores the outer scope");
    scope.end(stack + 8);
    expect(scope.use_frame_allocator(mem.reader(), base, a), "an unmatched end cannot clear an active scope");
    scope.end(stack);
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "allocation override ends after collector join");
    scope.begin(stack, collector); scope.begin(stack, 0);
    expect(!scope.use_frame_allocator(mem.reader(), base, a), "a sibling clears an abandoned scope");
    thread_scope.begin(stack, collector);
    bool other_thread_redirected = true;
    std::thread worker([&] { other_thread_redirected = thread_scope.use_frame_allocator(mem.reader(), base, a); });
    worker.join();
    expect(!other_thread_redirected && thread_scope.use_frame_allocator(mem.reader(), base, a), "worker threads cannot inherit a render-thread allocator scope");
    thread_scope.end(stack);
    const auto check_path = [&]<size_t Path>() {
        scope.begin(stack, collector);
        auto before = a;
        expect(m::redirect_allocation<Path>(mem.reader(), base, scope, a), "every allocation path selects its own cold branch");
        before.rip = base + m::allocation_paths[Path].fallback;
        expect(std::memcmp(&a, &before, sizeof(a)) == 0, "redirection preserves all registers, SIMD and stack; only RIP changes");
        scope.end(stack);
        before = a;
        expect(!m::redirect_allocation<Path>(mem.reader(), base, scope, a) && std::memcmp(&a, &before, sizeof(a)) == 0,
            "every path remains unchanged outside the validated gather scope");
    };
    check_path.operator()<0>(); check_path.operator()<1>(); check_path.operator()<2>(); check_path.operator()<3>();
}

void test_contracts(const wchar_t* path) {
    Memory mem;
    std::array<uint8_t, 512> pe{};
    mem.add(base, pe.data(), pe.size());
    mem.put(base, uint16_t{0x5A4D}); mem.put(base + 0x3C, uint32_t{0xC0});
    mem.put(base + 0xC0, uint32_t{0x4550}); mem.put(base + 0xC4, uint16_t{0x8664});
    mem.put(base + 0xC8, k::image_timestamp); mem.put(base + 0xD8, uint16_t{0x20B}); mem.put(base + 0x110, k::image_size);
    mem.add(base + 0xAFFF76, k::class_name_access.data(), k::class_name_access.size(), true);
    mem.add(base + 0xA1DC2F, k::object_iteration.data(), k::object_iteration.size(), true);
    mem.add(base + 0x58DC4E, k::name_entry_access.data(), k::name_entry_access.size(), true);
    for (const auto& e : m::code_evidence) { mem.add(base + e.rva, e.bytes.data(), e.bytes.size(), true); }
    for (const auto& p : m::allocation_paths) { mem.put(base + p.vtable, base + p.destructor); }
    expect(m::validate_code(mem.reader(), base), "complete gather, allocator, serial-loop and destructor contracts validate");
    for (const auto& e : m::code_evidence) for (size_t i = 0; i != e.bytes.size(); ++i) {
        mem.put(base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
        expect(!m::validate_code(mem.reader(), base), "every changed contract byte prevents installation");
        mem.put(base + e.rva + i, e.bytes[i]);
    }
    for (const auto& p : m::allocation_paths) {
        mem.put(base + p.vtable, base + p.destructor + 1);
        expect(!m::validate_code(mem.reader(), base), "a mismatched resource destructor prevents all lifetime hooks");
        mem.put(base + p.vtable, base + p.destructor);
    }
    mem.put(base + 0xC8, uint32_t{k::image_timestamp + 1});
    expect(!m::validate_code(mem.reader(), base), "unknown game builds cannot inherit collector offsets");
    expect(f::supports(L"SuicideSquad_KTJL.exe", true) && !f::supports(L"SuicideSquad_KTJL.exe", false) &&
        !f::supports(L"Other.exe", true), "resource repair is KTJL DX12 only");
    if (path) {
        std::ifstream file(std::filesystem::path{path}, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
        Memory captured; captured.add(base, bytes.data(), bytes.size(), true);
        expect(file.is_open() && m::validate_code(captured.reader(), base), "all contracts match the actual flat runtime image");
    }
}

void test_activation() {
    struct Hook {
        bool enabled{}, fail_enable{}, fail_disable{};
        bool enable() { if (fail_enable) { return false; } enabled = true; return true; }
        bool disable() { if (fail_disable) { return false; } enabled = false; return true; }
    };
    for (size_t failure = 0; failure != m::hook_count; ++failure) {
        std::array<Hook, m::hook_count> hooks{}; hooks[failure].fail_enable = true;
        expect(m::enable_hooks(hooks) == m::Activation::failed, "partial installation cannot publish readiness");
        expect(std::none_of(hooks.begin(), hooks.end(), [](const auto& h) { return h.enabled; }), "partial activation rolls back all previously enabled boundaries");
    }
    std::array<Hook, m::hook_count> hooks{};
    expect(m::enable_hooks(hooks) == m::Activation::ready && std::all_of(hooks.begin(), hooks.end(), [](const auto& h) { return h.enabled; }),
        "all boundaries are required before readiness");
    hooks = {}; hooks[0].fail_disable = true; hooks[1].fail_enable = true;
    expect(m::enable_hooks(hooks) == m::Activation::rollback_failed, "failed rollback still cannot publish readiness");
}

uintptr_t execution_base{};
bool force_serial{};
std::array<size_t, m::allocation_paths.size()> allocation_callbacks{}, redirected_callbacks{};
size_t end_callbacks{};
m::GatherScope<> execution_scope{};
void (*clobber)(){};
void begin_callback(safetyhook::Context& c) { clobber(); if (force_serial) { m::use_serial(c); } }
template<size_t Path> void allocation_callback(safetyhook::Context& c) {
    ++allocation_callbacks[Path];
    clobber();
    if (m::redirect_allocation<Path>(sdk::discovery::process_memory(), execution_base, execution_scope, c)) { ++redirected_callbacks[Path]; }
}
void end_callback(safetyhook::Context&) { ++end_callbacks; clobber(); }

uint32_t observed_num{}, observed_flags{};
int32_t observed_threads{};
std::array<uintptr_t, 2> observed_work{};
uint32_t parallel_observer(uint32_t num, const uintptr_t* work, uint32_t flags, const void*, int32_t threads) {
    observed_num = num; observed_flags = flags; observed_threads = threads;
    std::memcpy(observed_work.data(), work, sizeof(observed_work));
    return flags;
}

template<class T, size_t N> void put(std::array<uint8_t, N>& bytes, size_t at, const T& value) {
    std::memcpy(bytes.data() + at, &value, sizeof(value));
}

void test_private_allocator() {
    auto* reservation = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS));
    expect(reservation != nullptr, "padding-policy fixture reserves the complete near window");
    if (!reservation) { return; }
    auto* page = reservation + 0x8000;
    if (!VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
        expect(false, "padding-policy fixture commits");
        VirtualFree(reservation, 0, MEM_RELEASE); return;
    }
    std::memset(page, 0xCC, 0x1000);
    DWORD old{};
    expect(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old), "padding-policy fixture is executable");
    {
        auto legacy = safetyhook::Allocator::create();
        auto padding = legacy->allocate_near({page + 0x800}, 16, 0x1000);
        expect(padding && padding->data() >= page && padding->data() + padding->size() <= page + 0x1000,
            "default allocator retains the existing executable-padding fallback");
        auto private_pool = m::make_hook_allocator();
        auto refused = private_pool->allocate_near({page + 0x800}, 16, 0x1000);
        expect(!refused && refused.error() == safetyhook::Allocator::Error::NO_MEMORY_IN_RANGE,
            "KTJL allocator rejects padding when no private near allocation is available");
        auto far_memory = private_pool->allocate(64);
        MEMORY_BASIC_INFORMATION info{};
        expect(far_memory && VirtualQuery(far_memory->data(), &info, sizeof(info)) == sizeof(info) &&
            info.State == MEM_COMMIT && info.Type == MEM_PRIVATE && info.AllocationBase != reservation,
            "KTJL far trampoline uses an owned private allocation");
        if (padding) { padding->free(); }
        auto refused_again = private_pool->allocate_near({page + 0x800}, 16, 0x1000);
        expect(!refused_again, "KTJL pool cannot inherit freed image padding from the global/default policy");
        expect(std::all_of(page, page + 0x1000, [](uint8_t b) { return b == 0xCC; }),
            "allocation alone never alters the executable padding fixture");
    }
    VirtualFree(reservation, 0, MEM_RELEASE);
}

void test_execution(bool far_placement, bool available_padding = false) {
    // Reserve address space, not physical memory. The far case excludes the
    // entire rel32 allocator window around every hook; only fixture pages commit.
    const size_t reserve_size = size_t{k::image_size} + (far_placement ? 0x100010000ull : 0x10000ull);
    auto* reservation = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reservation != nullptr, "mesh fixture reserves address space");
    if (!reservation) { return; }
    auto* image = reservation + (far_placement ? 0x80000000ull : 0);
    execution_base = reinterpret_cast<uintptr_t>(image);
    std::vector<uintptr_t> pages;
    auto write = [&](uintptr_t rva, const void* bytes, size_t size) {
        for (uintptr_t page = rva & ~uintptr_t{0xFFF}; page <= ((rva + size - 1) & ~uintptr_t{0xFFF}); page += 0x1000) {
            if (std::find(pages.begin(), pages.end(), page) != pages.end()) { continue; }
            if (!VirtualAlloc(image + page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) { return false; }
            pages.push_back(page);
        }
        std::memcpy(image + rva, bytes, size);
        return true;
    };
    bool written = true;
    for (const auto& e : m::code_evidence) {
        written = write(e.rva, e.bytes.data(), e.bytes.size()) && written;
    }
    // The real fallback calls Windows TLS and the real resource destructor. Only
    // the unrelated ParallelFor task body is replaced by an argument observer.
    const auto tls_get = &TlsGetValue;
    written = write(0x69068A8, &tls_get, sizeof(tls_get)) && written;
    for (const auto& p : m::allocation_paths) {
        const auto destructor = execution_base + p.destructor;
        written = write(p.vtable, &destructor, sizeof(destructor)) && written;
    }
    const auto worker_tls = TlsAlloc(), frame_tls = TlsAlloc();
    expect(worker_tls != TLS_OUT_OF_INDEXES && frame_tls != TLS_OUT_OF_INDEXES, "fixture owns independent worker/frame TLS slots");
    if (worker_tls == TLS_OUT_OF_INDEXES || frame_tls == TLS_OUT_OF_INDEXES) {
        if (worker_tls != TLS_OUT_OF_INDEXES) { TlsFree(worker_tls); }
        if (frame_tls != TLS_OUT_OF_INDEXES) { TlsFree(frame_tls); }
        VirtualFree(reservation, 0, MEM_RELEASE); return;
    }
    written = write(0x8C0A5E0, &frame_tls, sizeof(frame_tls)) && written;
    std::array<uint8_t, 12> jump{0x48,0xB8};
    const auto observer = reinterpret_cast<uintptr_t>(&parallel_observer);
    std::memcpy(jump.data() + 2, &observer, 8); jump[10] = 0xFF; jump[11] = 0xE0;
    written = write(0x540628, jump.data(), jump.size()) && written;
    const uint8_t clobber_code[]{0x31,0xC9,0x31,0xD2,0x45,0x31,0xC0,0x45,0x31,0xC9,
        0x45,0x31,0xD2,0x45,0x31,0xDB,0x66,0x0F,0xEF,0xED,0xC3};
    written = write(0x700000, clobber_code, sizeof(clobber_code)) && written;
    clobber = reinterpret_cast<void(*)()>(image + 0x700000);
    const uint8_t end_prefix[]{0x55,0x57,0x41,0x56,0x48,0x81,0xEC,0,1,0,0,
        0x48,0x8D,0x6C,0x24,0x20,0xC7,0x44,0x24,0x70,7,0,0,0,
        0x48,0x89,0x4C,0x24,0x68,0xF3,0x41,0x0F,0x6F,0x28};
    std::array<uint8_t, 0x162> end_padding{}; end_padding.fill(0x90);
    std::memcpy(end_padding.data(), end_prefix, sizeof(end_prefix));
    written = write(0x75EE00, end_padding.data(), end_padding.size()) && written;
    const uint8_t end_tail[]{0x48,0x89,0x02,0x48,0x89,0x7A,0x08,0x4C,0x89,0x72,0x10,
        0x8B,0x85,0xC8,0,0,0,0x48,0x89,0x42,0x18,0xF3,0x0F,0x7F,0x6A,0x20,
        0x48,0x81,0xC4,0,1,0,0,0x41,0x5E,0x5F,0x5D,0xC3};
    written = write(m::end_rva + m::end_code.size(), end_tail, sizeof(end_tail)) && written;
    constexpr std::array<std::pair<uintptr_t, size_t>, 6> padding_spans{{
        {0x540F00, 64}, {0x6A2F00, 64}, {0x6AD114, 12}, {0x75EFE0, 32}, {0x6385F00, 64}, {0x6386F20, 64}}};
    if (available_padding) {
        std::array<uint8_t, 64> padding{}; padding.fill(0xCC);
        for (const auto& [rva, size] : padding_spans) { written = write(rva, padding.data(), size) && written; }
    }
    const auto padding_unchanged = [&] {
        if (!available_padding) { return true; }
        return std::all_of(padding_spans.begin(), padding_spans.end(), [&](const auto& span) {
            return std::all_of(image + span.first, image + span.first + span.second, [](uint8_t b) { return b == 0xCC; });
        });
    };
    expect(written, "all machine-code fixture pages commit");
    if (written) {
        for (auto page : pages) {
            DWORD old{};
            expect(VirtualProtect(image + page, 0x1000, PAGE_EXECUTE_READ, &old), "fixture pages become read-only executable");
        }
        FlushInstructionCache(GetCurrentProcess(), image, k::image_size);
        std::array<uint8_t, 0x330> object{};
        std::array<uint8_t, 0x1A60> worker{};
        std::array<uint8_t, 0x40> memstack{};
        std::vector<uint8_t> arena(64 * 24), frame_arena(8192);
        std::array<uintptr_t, 128> arena_resources{}, frame_resources{};
        put(object, 0x1C0, f::Header{reinterpret_cast<uintptr_t>(arena_resources.data()), 64, 128});
        put(object, 0x1E0, f::Header{reinterpret_cast<uintptr_t>(frame_resources.data()), 0, 128});
        put(object, 0x220, f::Header{reinterpret_cast<uintptr_t>(arena.data()), static_cast<int32_t>(arena.size()), static_cast<int32_t>(arena.size())});
        put(object, 0x2DC, worker_tls);
        put(memstack, 0x10, reinterpret_cast<uintptr_t>(frame_arena.data()));
        put(memstack, 0x18, reinterpret_cast<uintptr_t>(frame_arena.data() + frame_arena.size()));
        expect(TlsSetValue(worker_tls, worker.data()) && TlsSetValue(frame_tls, memstack.data()), "real fallback TLS is initialized");
        using Allocate = uintptr_t*(*)(void*);
        const auto allocate = [&](size_t path) { return reinterpret_cast<Allocate>(image + m::allocation_paths[path].allocator)(object.data()); };
        const auto destroy = [](uintptr_t* resource) {
            using Destroy = uintptr_t(*)(void*, uint32_t);
            reinterpret_cast<Destroy>(*reinterpret_cast<uintptr_t*>(resource[0]))(resource, 0);
        };
        using Parallel = uint32_t(*)(uint32_t, const uintptr_t*, uint32_t, uintptr_t, int32_t);
        const auto parallel = reinterpret_cast<Parallel>(image + m::begin_rva);
        using End = void(*)(const void*, uint64_t*, const uint64_t*);
        const auto end = reinterpret_cast<End>(image + 0x75EE00);
        execution_scope = {}; force_serial = false; allocation_callbacks = {}; redirected_callbacks = {}; end_callbacks = 0;
        {
            const auto allocator = m::make_hook_allocator();
            auto hooks = m::prepare_hooks(execution_base, {begin_callback,
                allocation_callback<0>, allocation_callback<1>, allocation_callback<2>, allocation_callback<3>, end_callback},
                [&](void* at, safetyhook::MidHookFn fn, safetyhook::MidHook::Flags flags) {
                    return safetyhook::MidHook::create(allocator, at, fn, flags);
                });
            expect(!hooks.error, "all exact hook boundaries prepare in near and exhausted-near-space layouts");
            expect(padding_unchanged(), "preparing the exact production hooks leaves all executable padding unchanged");
            if (!hooks.error) {
                if (far_placement) for (const auto& hook : hooks.hooks) {
                    expect(hook.original_bytes().size() >= 14, "far test actually exercises the 14-byte trampoline form");
                }
                const std::array<uintptr_t, 2> body{0x12345678, 0xABCDEF00};
                expect(parallel(37, body.data(), 4, 0, -1) == 4 && allocation_callbacks == decltype(allocation_callbacks){},
                    "preparation leaves the original code disabled and unchanged");
                expect(m::enable_hooks(hooks.hooks) == m::Activation::ready, "all six machine-code hooks enable together");
                expect(padding_unchanged(), "enabling the hooks never writes a trampoline into game padding");
                for (bool on : {false, true}) for (uint32_t flags : {0u, 1u, 4u, 5u}) {
                    force_serial = on;
                    expect(parallel(37, body.data(), flags, 0, -1) == (flags | (on ? 1u : 0u)), "the real wrapper forwards the intended serial flag");
                    expect(observed_num == 37 && observed_work == body && observed_threads == -1 && observed_flags == (flags | (on ? 1u : 0u)),
                        "the wrapper preserves original task identity, count and thread limit despite callback register clobbering");
                }
                std::array<uint64_t, 6> end_out{};
                const std::array<uint64_t, 2> vector{0x123456789ABCDEF0,0xFEDCBA9876543210};
                end(arena_resources.data(), end_out.data(), vector.data());
                expect(end_callbacks == 1 && end_out[0] == 7 && end_out[1] == reinterpret_cast<uintptr_t>(arena_resources.data()) &&
                    end_out[2] == end_out[1] + 56 && end_out[3] == 0 && end_out[4] == vector[0] && end_out[5] == vector[1],
                    "the end hook preserves the real stack loads, zero store, vector state and epilogue");
                size_t offset{};
                for (size_t path = 0; path != m::allocation_paths.size(); ++path) {
                    auto* resource = allocate(path);
                    expect(resource == reinterpret_cast<uintptr_t*>(arena.data() + offset) && arena_resources[path] == reinterpret_cast<uintptr_t>(resource) &&
                        resource[0] == execution_base + m::allocation_paths[path].vtable && resource[1] == 0 &&
                        (m::allocation_paths[path].size == 16 || resource[2] == 0),
                        "unselected calls retain each exact 16/24-byte scratch allocator and constructor");
                    offset += m::allocation_paths[path].size;
                    destroy(resource);
                }
                const auto old_resource = arena_resources[0];
                std::vector<uint8_t> replacement(96 * 24);
                arena.swap(replacement); replacement.clear(); replacement.shrink_to_fit();
                expect(old_resource < reinterpret_cast<uintptr_t>(arena.data()) ||
                    old_resource >= reinterpret_cast<uintptr_t>(arena.data() + arena.size()),
                    "resizing the shared scratch arena invalidates the old resource identity");
                arena_resources.fill(0); worker.fill(0);
                put(object, 0x220, f::Header{reinterpret_cast<uintptr_t>(arena.data()), static_cast<int32_t>(arena.size()), static_cast<int32_t>(arena.size())});
                const auto scope_stack = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
                execution_scope.begin(scope_stack, reinterpret_cast<uintptr_t>(object.data()));
                std::array<std::array<uint64_t, 4>, 100> rhi_refs{};
                std::vector<uintptr_t*> resources;
                for (size_t eye = 0; eye != 2; ++eye) {
                    const size_t count = eye == 0 ? 30 : 70;
                    arena = std::vector<uint8_t>((eye ? 90 : 32) * 24, 0xCD);
                    put(object, 0x220, f::Header{reinterpret_cast<uintptr_t>(arena.data()), static_cast<int32_t>(arena.size()), static_cast<int32_t>(arena.size())});
                    worker.fill(0);
                    for (size_t i = 0; i < count; ++i) {
                        const size_t index = resources.size();
                        const auto& path = m::allocation_paths[index % m::allocation_paths.size()];
                        auto* resource = allocate(index % m::allocation_paths.size());
                        const auto address = reinterpret_cast<uintptr_t>(resource);
                        expect(address >= reinterpret_cast<uintptr_t>(frame_arena.data()) && address + path.size <= reinterpret_cast<uintptr_t>(frame_arena.data() + frame_arena.size()) &&
                            resource[0] == execution_base + path.vtable && resource[1] == 0 && (path.size == 16 || resource[2] == 0),
                            "all four original fallback constructors use frame storage, correct alignment, size and resource type");
                        rhi_refs[index][1] = 2;
                        resource[1] = index + 1;
                        if (path.size == 24) { resource[2] = reinterpret_cast<uintptr_t>(rhi_refs[index].data()); }
                        resources.push_back(resource);
                    }
                }
                execution_scope.end(scope_stack);
                expect(std::all_of(redirected_callbacks.begin(), redirected_callbacks.end(), [](auto n) { return n == 25; }),
                    "all four production redirection paths execute for both eyes");
                expect(std::all_of(worker.begin(), worker.end(), [](auto b) { return b == 0; }),
                    "frame fallback never consumes the per-view arena or cleanup cursor");
                expect(resources.size() == 100 && std::all_of(arena_resources.begin(), arena_resources.end(), [](auto p) { return p == 0; }),
                    "the repaired path leaves no stale resource pointers in the per-view cleanup array");
                f::Header registered{}; std::memcpy(&registered, object.data() + 0x1E0, sizeof(registered));
                expect(registered.count == 100, "the real fallback registers every resource with the original collector destructor list");
                for (size_t i = 0; i != resources.size(); ++i) {
                    auto* resource = resources[i];
                    expect(frame_resources[i] == reinterpret_cast<uintptr_t>(resource) && resource[1] == i + 1 && rhi_refs[i][1] == 2,
                        "both eyes retain their own untouched resources after the scratch storage is replaced");
                    destroy(resource);
                    expect(resource[0] == execution_base + 0x69851E0 && rhi_refs[i][1] == (m::allocation_paths[i % 4].size == 24 ? 1 : 2),
                        "each real virtual destructor releases only its own RHI reference exactly once without freeing frame memory");
                }
                const auto redirects_before = redirected_callbacks;
                for (size_t path = 0; path != m::allocation_paths.size(); ++path) {
                    auto* resource = allocate(path);
                    expect(arena_resources[path] == reinterpret_cast<uintptr_t>(resource), "scope exit restores original scratch allocation for every type");
                    destroy(resource);
                }
                expect(redirected_callbacks == redirects_before, "no type inherits a stale scope after the collector join");
                expect(TlsSetValue(worker_tls, nullptr), "fixture clears worker TLS");
                for (size_t path = 0; path != m::allocation_paths.size(); ++path) {
                    auto* resource = allocate(path);
                    expect(reinterpret_cast<uintptr_t>(resource) >= reinterpret_cast<uintptr_t>(frame_arena.data()) &&
                        reinterpret_cast<uintptr_t>(resource) + m::allocation_paths[path].size <= reinterpret_cast<uintptr_t>(frame_arena.data() + frame_arena.size()),
                        "the original null-TLS cold branch remains usable without any redirection");
                    destroy(resource);
                }
                for (auto& hook : hooks.hooks) { expect(hook.disable().has_value(), "machine-code hook uninstalls cleanly"); }
                expect(parallel(37, body.data(), 4, 0, -1) == 4, "uninstall restores original scheduling");
                expect(padding_unchanged(), "original padding remains intact after callback execution and uninstall");
            }
        }
        TlsSetValue(worker_tls, nullptr); TlsSetValue(frame_tls, nullptr);
    }
    TlsFree(worker_tls); TlsFree(frame_tls);
    VirtualFree(reservation, 0, MEM_RELEASE);
}
}

int test_ktjl_mesh_resources(const wchar_t* path) {
    test_selection(); test_scope(); test_contracts(path); test_activation();
    test_private_allocator();
    test_execution(false); test_execution(true); test_execution(true, true);
    std::cout << "KTJL mesh resource lifetime: " << failures << " failures\n";
    return failures;
}
