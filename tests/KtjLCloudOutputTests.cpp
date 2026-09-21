#define NOMINMAX
#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include "mods/vr/KtjLCloudOutputHook.hpp"

namespace {
namespace c = uevr::ktjl::cloud_output;
namespace f = uevr::ktjl::fog;
namespace k = sdk::ktjl;
constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
constexpr uintptr_t state = 0x62000000, rhi = 0x63000000, stack = 0x64000000;
constexpr uintptr_t resource_base = 0x65000000;
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

struct Memory {
    struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool executable{}; };
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

Memory fixture() {
    Memory m;
    m.put(renderer + f::views_offset, f::Header{views, 2, 2});
    for (size_t i = 0; i != 2; ++i) {
        const auto s = state + i * 0x10000;
        m.put(views + i * f::view_stride, f::ViewPrefix{base + f::view_vtable_rva, 0, renderer + 0x10, s});
        m.put(views + i * f::view_stride + uevr::ktjl::cloud::state_offset, s);
        m.put(s, base + k::stereo::state_vtable_rva);
        for (const auto slot : c::slots) { m.put(s + slot, uintptr_t{}); }
    }
    m.put(rhi + 0xD0, uintptr_t{});
    m.put(base + c::targets_rva + 0x514, std::array<int32_t, 2>{4944, 2416});
    m.put(base + c::quality_rva, uint32_t{0});
    m.put(base + c::flags_rva, uint32_t{0x100000});
    m.put(stack, base + c::producer_return_rva);
    return m;
}

void prepare_eye(Memory& m, size_t eye, const c::Extent& size) {
    for (size_t slot = 0; slot != 2; ++slot) {
        const auto address = resource_base + (eye * 2 + slot) * 0x10000;
        std::array<uint8_t, 0xF0> bytes{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(bytes.data() + offset, &value, sizeof(value)); };
        put(0, base + f::pool_vtable_rva); put(8, address + 0x1000); put(0x10, address + 0x1000);
        put(0x18, address + 0x2000); put(0x88, int32_t{2}); put(0xE8, base + f::pool_rva);
        put(0x90 + 0x14, size.width); put(0x90 + 0x18, size.height); put(0x90 + 0x20, int32_t{1});
        put(0x90 + 0x26, uint16_t{1}); put(0x90 + 0x28, uint16_t{1}); put(0x90 + 0x2C, c::formats[slot]);
        put(0x90 + 0x30, slot == 0 ? size.flags : 0u); put(0x90 + 0x34, c::target_flags[slot]);
        m.put(address, bytes);
        m.put(state + eye * 0x10000 + c::slots[slot], address);
    }
}

template<class Prepare>
c::Outcome prepare(Memory& m, Prepare&& operation) {
    return c::prepare_secondary(m.reader(), base, rhi, views, renderer, rhi, renderer + f::views_offset, stack, operation);
}

void test_lifetime() {
    auto m = fixture();
    const auto initial = *c::read_extent(m.reader(), base, rhi);
    expect(initial.width == 1236 && initial.height == 604, "extent uses the game's context and downsample quality");
    prepare_eye(m, 0, initial);
    auto saved_primary = m.blocks;
    m.put(stack, base + c::consumer_returns[0]);
    expect(c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "captured null-right-resource crash is rejected before acquiring output refs");
    expect(!c::reject_consumer(m.reader(), base, views, stack), "primary consumer is never changed");
    m.put(stack, base + c::producer_return_rva);
    int calls{};
    const auto engine = [&](uintptr_t command, uintptr_t view) {
        ++calls;
        expect(command == rhi && view == views + f::view_stride, "only the matching right-eye producer is called");
        expect(c::prepare_secondary(m.reader(), base, command, view, renderer, rhi, renderer + f::views_offset, stack,
            [](auto, auto) { expect(false, "secondary producer must not recurse"); return false; }) == c::Outcome::passthrough,
            "calling the engine producer for the second eye is bounded");
        prepare_eye(m, 1, *c::read_extent(m.reader(), base, command));
        return true;
    };
    expect(prepare(m, engine) == c::Outcome::prepared && calls == 1, "one engine call prepares both missing resources");
    for (int i = 0; i != 20; ++i) { expect(prepare(m, engine) == c::Outcome::existing, "stable frames do not reallocate or replay the producer"); }
    expect(calls == 1, "stable preparation remains bounded and allocation-free");
    for (const auto caller : c::consumer_returns) {
        m.put(stack, base + caller);
        expect(!c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "both compute and composition consumers receive distinct valid resources");
    }
    for (const auto& b : saved_primary) {
        if ((b.address >= state && b.address < state + 0x10000) || (b.address >= resource_base && b.address < resource_base + 0x20000)) {
            std::vector<uint8_t> after(b.bytes.size());
            expect(m.reader().read(&m, b.address, after.data(), after.size()) && after == b.bytes, "primary slots, resources and reference counts are untouched");
        }
    }
    m.put(stack, base + c::producer_return_rva);
    m.put(base + c::targets_rva + 0x514, std::array<int32_t, 2>{7680, 4320});
    expect(prepare(m, engine) == c::Outcome::prepared && calls == 2, "resolution change reuses the engine's replacement/retirement path");
    m.put(stack, base + c::consumer_returns[1]);
    expect(c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "consumer refuses an incomplete eye resize");
    prepare_eye(m, 0, *c::read_extent(m.reader(), base, rhi));
    expect(!c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "consumer resumes when both resized resources validate");
    for (const auto slot : c::slots) { m.put(state + 0x10000 + slot, uintptr_t{}); }
    expect(c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "released resources are not cached across travel");
    m.put(stack, base + c::producer_return_rva);
    expect(prepare(m, engine) == c::Outcome::prepared && calls == 3, "cleared view-state resources are prepared on the next frame");
}

void test_rejections() {
    const auto fail_call = [](auto, auto) { expect(false, "invalid/unrelated context must not invoke the producer"); return false; };
    for (int mode = 0; mode != 13; ++mode) {
        auto m = fixture();
        if (mode == 0) { m.put(renderer + f::views_offset, f::Header{views, 1, 2}); }
        if (mode == 1) { m.put(renderer + f::views_offset, f::Header{views, 3, 3}); }
        if (mode == 2) { m.put(renderer + f::views_offset, f::Header{views, 2, 1}); }
        if (mode == 3) { m.put(renderer + f::views_offset, f::Header{views, 2, 17}); }
        if (mode == 4) { m.put(views, uintptr_t{base + f::view_vtable_rva + 8}); }
        if (mode == 5) { m.put(state, uintptr_t{base + k::stereo::state_vtable_rva + 8}); }
        if (mode == 6) { m.put(views + f::view_stride + 0x10, uintptr_t{renderer + 0x20}); }
        if (mode == 7) { m.put(views + f::view_stride + uevr::ktjl::cloud::state_offset, state); }
        if (mode == 8) { m.put(stack, uintptr_t{base + c::producer_return_rva + 1}); }
        if (mode == 9) { m.put(base + c::quality_rva, uint32_t{3}); }
        if (mode == 10) { m.put(base + c::targets_rva + 0x514, std::array<int32_t,2>{0,1080}); }
        if (mode == 11) { m.put(rhi + 0xD0, uintptr_t{1}); }
        if (mode == 12) { m.put(state + 0x10000 + c::slots[0], uintptr_t{1}); }
        const auto result = prepare(m, fail_call);
        expect(result == c::Outcome::passthrough || result == c::Outcome::rejected, "malformed or unrelated pair fails without a call");
    }
    for (int reg = 0; reg != 3; ++reg) {
        auto m = fixture();
        expect(c::prepare_secondary(m.reader(), base, rhi, views, renderer + (reg == 0 ? 8 : 0), rhi + (reg == 1 ? 8 : 0),
            renderer + f::views_offset + (reg == 2 ? 8 : 0), stack, fail_call) == c::Outcome::passthrough,
            "all three preserved original caller registers must agree");
    }
    for (int mode = 0; mode != 8; ++mode) {
        auto m = fixture();
        const auto size = *c::read_extent(m.reader(), base, rhi);
        prepare_eye(m, 0, size);
        expect(prepare(m, [&](auto, auto) {
            if (mode == 0) { return false; }
            if (mode == 1) { return true; }
            prepare_eye(m, 1, size);
            const auto r = resource_base + 0x20000;
            if (mode == 2) { m.put(r, uintptr_t{base + f::pool_vtable_rva + 8}); }
            if (mode == 3) { m.put(r + 0x88, int32_t{1}); }
            if (mode == 4) { m.put(r + 0x18, uintptr_t{}); }
            if (mode == 5) { m.put(r + 0x90 + 0x2C, uint32_t{15}); }
            if (mode == 6) { m.put(r + 0x90 + 0x14, int32_t{32}); }
            if (mode == 7) { m.put(renderer + f::views_offset, f::Header{views, 1, 2}); }
            return true;
        }) == c::Outcome::rejected, "failure, partial allocation, wrong texture and generation changes never publish success");
    }
    auto m = fixture();
    const auto size = *c::read_extent(m.reader(), base, rhi);
    prepare_eye(m, 0, size); prepare_eye(m, 1, size);
    m.put(stack, base + c::consumer_returns[0]);
    m.put(state + 0x10000 + c::slots[0], resource_base);
    expect(c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "aliasing the primary eye is never accepted");
    m.put(stack, base + c::consumer_returns[0] + 1);
    expect(!c::reject_consumer(m.reader(), base, views + f::view_stride, stack), "unknown callers retain original semantics");
    m = fixture();
    m.put(rhi + 0xD0, uintptr_t{0x66000000});
    m.put(0x66000514, std::array<int32_t, 2>{2560, 1440});
    m.put(base + c::quality_rva, uint32_t{2});
    expect(c::read_extent(m.reader(), base, rhi)->width == 2560, "explicit RHI context overrides global scene targets");
}

Memory contract_fixture() {
    Memory m;
    std::array<uint8_t, 512> pe{};
    m.add(base, pe.data(), pe.size());
    m.put(base, uint16_t{0x5A4D}); m.put(base + 0x3C, uint32_t{0xC0});
    m.put(base + 0xC0, uint32_t{0x4550}); m.put(base + 0xC4, uint16_t{0x8664});
    m.put(base + 0xC8, k::image_timestamp); m.put(base + 0xD8, uint16_t{0x20B}); m.put(base + 0x110, k::image_size);
    const auto code = [&](uintptr_t rva, const auto& bytes) { m.add(base + rva, bytes.data(), bytes.size(), true); };
    code(0xAFFF76, k::class_name_access); code(0xA1DC2F, k::object_iteration); code(0x58DC4E, k::name_entry_access);
    for (const auto& e : f::code_evidence) { code(e.rva, e.bytes); }
    for (const auto& e : uevr::ktjl::cloud::code_evidence) { code(e.rva, e.bytes); }
    for (const auto& e : c::code_evidence) { code(e.rva, e.bytes); }
    m.put(base + f::pool_vtable_rva + 0x10, base + f::get_desc_rva);
    m.put(base + f::pool_vtable_rva + 0x38, uintptr_t{base + 0x55AAE0});
    m.put(base + f::view_vtable_rva, uintptr_t{base + 0x57836D0});
    m.put(base + k::stereo::state_vtable_rva, uintptr_t{base + 0x1986F94});
    return m;
}

void test_contracts(const wchar_t* path) {
    expect(f::supports(L"D:\\Games\\SuicideSquad_KTJL.exe", true) && !f::supports(L"Other.exe", true) &&
        !f::supports(L"SuicideSquad_KTJL.exe", false), "cloud output repair is exact title and DX12 only");
    auto offline = contract_fixture();
    expect(c::validate_code(offline.reader(), base), "offline producer, consumer and ownership contracts validate");
    for (const auto& e : c::code_evidence) {
        for (size_t i = 0; i != e.bytes.size(); ++i) {
            offline.put(base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!c::validate_code(offline.reader(), base), "CI rejects every changed producer/consumer contract byte");
            offline.put(base + e.rva + i, e.bytes[i]);
        }
        for (bool truncate : {false, true}) {
            auto invalid = offline;
            for (auto& b : invalid.blocks) {
                if (b.address == base + e.rva) { if (truncate) { b.bytes.pop_back(); } else { b.executable = false; } }
            }
            expect(!c::validate_code(invalid.reader(), base), "truncated and non-executable code refuses installation");
        }
    }
    if (!path) { return; }
    std::ifstream file(std::filesystem::path{path}, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    Memory m;
    m.add(base, bytes.data(), bytes.size(), true);
    expect(file.is_open() && c::validate_code(m.reader(), base), "producer, consumers, allocator and cleanup match the captured executable");
    for (const auto& e : c::code_evidence) {
        for (size_t i = 0; i != e.bytes.size(); ++i) {
            m.put(base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!c::validate_code(m.reader(), base), "every contract byte rejects a changed game image");
            m.put(base + e.rva + i, e.bytes[i]);
        }
    }
    uint32_t nt{};
    expect(m.reader().load(base + 0x3C, nt), "runtime PE header location is readable");
    m.put(base + nt + 8, uint32_t{k::image_timestamp + 1});
    expect(!c::validate_code(m.reader(), base), "updated image cannot reuse this fixed contract");
}

using NativeCall = uint64_t(*)(uint64_t*, uint64_t, const uint64_t*, uint64_t);
NativeCall native_producer{};
uintptr_t native_base{};
bool block_consumer{};
void (*clobber)(){};
void producer_callback(safetyhook::Context& ctx) {
    if (ctx.rdx == 1) {
        native_producer(reinterpret_cast<uint64_t*>(ctx.rcx), 2, reinterpret_cast<const uint64_t*>(ctx.r8), 0xDDDD);
    }
    clobber();
}
void consumer_callback(safetyhook::Context& ctx) {
    clobber();
    if (block_consumer) { c::skip_unprepared_secondary(ctx, native_base); }
}

void test_native_hooks(bool exhausted_near_space) {
    const size_t size = exhausted_near_space ? 0x110000000ull : 0x400000;
    auto* reserved = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reserved != nullptr, "resource hook test reserves bounded address space");
    if (!reserved) { return; }
    auto* producer_page = reserved + (exhausted_near_space ? 0x88000000ull : 0x200000);
    native_base = reinterpret_cast<uintptr_t>(producer_page) - (c::producer_rva & ~uintptr_t{0xFFF});
    auto* consumer_page = reinterpret_cast<uint8_t*>((native_base + c::consumer_rva) & ~uintptr_t{0xFFF});
    auto* producer = reinterpret_cast<uint8_t*>(native_base + c::producer_rva);
    auto* consumer = reinterpret_cast<uint8_t*>(native_base + c::consumer_rva);
    if (!VirtualAlloc(producer_page, 0x1000, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(consumer_page, 0x1000, MEM_COMMIT, PAGE_READWRITE)) {
        expect(false, "two resource test pages commit"); VirtualFree(reserved, 0, MEM_RELEASE); return;
    }
    std::memset(producer_page, 0x90, 0x1000); std::memset(consumer_page, 0x90, 0x1000);
    // Execute the exact relocatable entry instructions. The body records the
    // arguments/vector visible after the callback, then restores original saves.
    const uint8_t body[]{0x4C,0x8B,0x11,0x4D,0x6B,0xD2,0x28,0x4E,0x8D,0x54,0x11,0x08,
        0x49,0x89,0x12,0x4D,0x89,0x42,0x08,0x4D,0x89,0x4A,0x10,0xF3,0x41,0x0F,0x7F,0x6A,0x18,
        0x48,0xFF,0x01,0xB8,0x47,0,0,0};
    std::memcpy(producer, c::producer_code.data(), 0x1C);
    std::memcpy(producer + 0x1C, body, sizeof(body));
    std::memcpy(producer + 0x1C + sizeof(body), c::producer_code.data() + 0x207, c::producer_code.size() - 0x207);
    std::memcpy(consumer, c::consumer_code.data(), 15);
    std::memcpy(consumer + 15, body, sizeof(body));
    consumer[15 + sizeof(body)] = 0xC3;
    *reinterpret_cast<uint8_t*>(native_base + c::consumer_ret_rva) = 0xC3;
    const uint8_t clobber_code[]{0x31,0xC9,0x31,0xD2,0x45,0x31,0xC0,0x45,0x31,0xC9,
        0x45,0x31,0xD2,0x45,0x31,0xDB,0x66,0x0F,0xEF,0xED,0xC3};
    std::memcpy(producer_page + 0x600, clobber_code, sizeof(clobber_code));
    clobber = reinterpret_cast<void(*)()>(producer_page + 0x600);
    const auto wrapper = [&](uint8_t* at, uint8_t* target) {
        const uint8_t prefix[]{0x48,0x83,0xEC,0x28,0xF3,0x41,0x0F,0x6F,0x28,0xE8};
        std::memcpy(at, prefix, sizeof(prefix));
        const auto rel = static_cast<int32_t>(target - (at + 14));
        std::memcpy(at + 10, &rel, 4);
        const uint8_t tail[]{0x48,0x83,0xC4,0x28,0xC3}; std::memcpy(at + 14, tail, sizeof(tail));
        return reinterpret_cast<NativeCall>(at);
    };
    auto call_producer = wrapper(producer_page + 0x100, producer);
    auto call_consumer = wrapper(producer_page + 0x200, consumer);
    native_producer = reinterpret_cast<NativeCall>(producer);
    DWORD old{};
    expect(VirtualProtect(producer_page, 0x1000, PAGE_EXECUTE_READ, &old) &&
        VirtualProtect(consumer_page, 0x1000, PAGE_EXECUTE_READ, &old), "native fixtures become read-only executable");
    FlushInstructionCache(GetCurrentProcess(), producer_page, 0x1000);
    FlushInstructionCache(GetCurrentProcess(), consumer_page, 0x1000);
    std::array<uint64_t, 2> vector{0x123456789ABCDEF0, 0xFEDCBA9876543210};
    std::array<uint64_t, 16> out{};
    expect(call_producer(out.data(), 1, vector.data(), 0x7777) == 71 && out[0] == 1 && out[1] == 1,
        "unmodified producer fixture runs once");
    {
        auto hooks = c::prepare_hooks(native_base, producer_callback, consumer_callback,
            [](void* at, safetyhook::MidHookFn callback, safetyhook::MidHook::Flags flags) {
                return safetyhook::MidHook::create(safetyhook::Allocator::create(), at, callback, flags);
            });
        expect(hooks.producer && hooks.consumer, "both exact entry hooks support constrained trampoline placement");
        if (hooks.producer && hooks.consumer) {
            if (exhausted_near_space) {
                expect(hooks.producer.original_bytes().size() >= 14 && hooks.consumer.original_bytes().size() >= 14,
                    "far-placement fixture actually uses absolute-entry trampolines");
            }
            expect(c::enable_pair(hooks.producer, hooks.consumer), "consumer and producer guards activate together");
            for (int i = 0; i != 4; ++i) {
                out = {};
                expect(call_producer(out.data(), 1, vector.data(), 0x7777) == 71 && out[0] == 2 && out[1] == 2 && out[6] == 1,
                    "secondary preparation is bounded and the original primary still runs exactly once");
                expect(out[7] == reinterpret_cast<uintptr_t>(vector.data()) && out[8] == 0x7777 &&
                    out[9] == vector[0] && out[10] == vector[1], "nested allocator-like calls cannot clobber primary arguments or SIMD state");
                for (bool reject : {false, true}) {
                    block_consumer = reject; out = {};
                    expect(call_consumer(out.data(), 1, vector.data(), 0x7777) == (reject ? 0 : 71) && out[0] == (reject ? 0 : 1),
                        "shared helper returns false without touching outputs only for rejected consumers");
                    if (!reject) {
                        expect(out[1] == 1 && out[2] == reinterpret_cast<uintptr_t>(vector.data()) && out[3] == 0x7777 &&
                            out[4] == vector[0] && out[5] == vector[1], "allowed helper preserves volatile arguments and vector values");
                    }
                }
            }
            expect(hooks.producer.disable().has_value() && hooks.consumer.disable().has_value(), "both resource guards restore cleanly");
            out = {};
            expect(call_producer(out.data(), 1, vector.data(), 0x7777) == 71 && out[0] == 1,
                "hook removal restores the primary-only producer");
        }
    }
    VirtualFree(reserved, 0, MEM_RELEASE);
}

void test_activation() {
    struct Hook {
        bool succeeds{true}, enabled{};
        size_t enables{}, disables{};
        bool enable() { ++enables; return enabled = succeeds; }
        bool disable() { ++disables; enabled = false; return true; }
    };
    Hook producer{}, consumer{};
    consumer.succeeds = false;
    expect(!c::enable_pair(producer, consumer) && producer.enables == 0 && !producer.enabled,
        "producer cannot activate without its consumer safety boundary");
    producer = {}; consumer = {}; producer.succeeds = false;
    expect(!c::enable_pair(producer, consumer) && consumer.disables == 1 && !consumer.enabled,
        "producer activation failure rolls back the consumer");
    producer = {}; consumer = {};
    expect(c::enable_pair(producer, consumer) && producer.enabled && consumer.enabled, "ready requires both resource boundaries");
    safetyhook::Context context{};
    context.rax = 0x12345678; context.rcx = 0x1357; context.rdx = 0x2468; context.rsp = 0x6789;
    c::skip_unprepared_secondary(context, base);
    expect(context.rax == 0 && context.rip == base + c::consumer_ret_rva &&
        context.rcx == 0x1357 && context.rdx == 0x2468 && context.rsp == 0x6789,
        "rejected helper changes only return value and continuation, not arguments or stack");
}
}

int test_ktjl_cloud_outputs(const wchar_t* path) {
    test_contracts(path);
    test_lifetime();
    test_rejections();
    test_activation();
    test_native_hooks(false);
    test_native_hooks(true);
    std::cout << "KTJL cloud output resources: " << failures << " failures\n";
    return failures;
}
