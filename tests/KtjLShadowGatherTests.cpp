#define NOMINMAX
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include "mods/vr/KtjLShadowGatherHook.hpp"

namespace {
namespace s = uevr::ktjl::shadow;
namespace f = uevr::ktjl::fog;
namespace k = sdk::ktjl;
int failures{};
constexpr uintptr_t base = 0x140000000, renderer = 0x60000000, views = 0x61000000;
constexpr uintptr_t states = 0x62000000, depth_view = 0x63000000, shadow = 0x64000000;
constexpr uintptr_t outer_stack = 0x65001008, frame_pointer = outer_stack - s::collect_saved_register_bytes;
constexpr uintptr_t gather_stack = frame_pointer - s::collect_stack_bytes - 8;
constexpr uintptr_t reused_views = frame_pointer - s::reused_views_frame_offset;

void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

struct Memory {
    struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool executable{}; };
    std::vector<Block> blocks{};
    uintptr_t native_stack{};
    void add(uintptr_t address, const void* p, size_t n, bool executable = false) {
        const auto* bytes = static_cast<const uint8_t*>(p);
        blocks.push_back({address, {bytes, bytes + n}, executable});
    }
    template<class T> void put(uintptr_t address, const T& value) {
        for (auto& b : blocks) {
            if (address >= b.address && address - b.address <= b.bytes.size() && sizeof(T) <= b.bytes.size() - (address - b.address)) {
                std::memcpy(b.bytes.data() + address - b.address, &value, sizeof(T)); return;
            }
        }
        add(address, &value, sizeof(T));
    }
    sdk::discovery::Memory reader() {
        return {this, [](void* self, uintptr_t a, void* out, size_t n) {
            auto& m = *static_cast<Memory*>(self);
            if (m.native_stack && a == m.native_stack && n == sizeof(uintptr_t)) {
                std::memcpy(out, reinterpret_cast<void*>(a), n); return true;
            }
            for (const auto& b : m.blocks) {
                if (a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) {
                    std::memcpy(out, b.bytes.data() + a - b.address, n); return true;
                }
            }
            return false;
        }, [](void* self, uintptr_t a, size_t n) {
            for (const auto& b : static_cast<Memory*>(self)->blocks) {
                if (b.executable && a >= b.address && a - b.address <= b.bytes.size() && n <= b.bytes.size() - (a - b.address)) { return true; }
            }
            return false;
        }};
    }
};

Memory fixture(uintptr_t image = base, uintptr_t object = shadow) {
    Memory m;
    std::array<uint8_t, 512> pe{};
    m.add(image, pe.data(), pe.size());
    m.put(image, uint16_t{0x5A4D}); m.put(image + 0x3C, uint32_t{0x80});
    m.put(image + 0x80, uint32_t{0x4550}); m.put(image + 0x84, uint16_t{0x8664});
    m.put(image + 0x88, k::image_timestamp); m.put(image + 0x98, uint16_t{0x20B}); m.put(image + 0xD0, k::image_size);
    const auto code = [&](uintptr_t rva, const auto& bytes) { m.add(image + rva, bytes.data(), bytes.size(), true); };
    code(0xAFFF76, k::class_name_access); code(0xA1DC2F, k::object_iteration); code(0x58DC4E, k::name_entry_access);
    for (const auto& e : s::code_evidence) { code(e.rva, e.bytes); }
    m.put(renderer + s::frame_offset, uint32_t{19112});
    m.put(renderer + f::views_offset, f::Header{views, 2, 2});
    for (size_t i = 0; i < 2; ++i) {
        m.put(views + i * f::view_stride, f::ViewPrefix{image + f::view_vtable_rva, 0, renderer + 0x10, states + i * 0x100});
        m.put(states + i * 0x100, image + k::stereo::state_vtable_rva);
    }
    m.put(object + s::depth_view_offset, depth_view);
    m.put(depth_view, f::ViewPrefix{image + f::view_vtable_rva, 0, renderer + 0x10, states});
    m.put(gather_stack, image + s::caller_returns[0]);
    return m;
}

template<size_t N, class Epoch>
s::Decision visit(s::CollectionScope<N, Epoch>& scope, Memory& m, uintptr_t object = shadow) {
    return scope.observe(m.reader(), base, renderer, object, gather_stack, frame_pointer, reused_views);
}

void test_contracts(const wchar_t* image_path) {
    auto m = fixture();
    expect(s::validate_code(m.reader(), base), "exact image, prologues, void call sites and bare RET validate");
    expect(f::supports(L"D:\\Games\\SuicideSquad_KTJL.exe", true) &&
        !f::supports(L"D:\\Games\\SuicideSquad_KTJL.exe", false) && !f::supports(L"Other.exe", true),
        "shadow repair remains exact-title and DX12 only");
    for (const auto& e : s::code_evidence) {
        for (size_t i = 0; i < e.bytes.size(); ++i) {
            auto changed = m;
            changed.put(base + e.rva + i, uint8_t(e.bytes[i] ^ 1));
            expect(!s::validate_code(changed.reader(), base), "every changed contract byte fails closed");
        }
        for (size_t size = 0; size < e.bytes.size(); ++size) {
            auto changed = m;
            for (auto& b : changed.blocks) { if (b.address == base + e.rva) { b.bytes.resize(size); } }
            expect(!s::validate_code(changed.reader(), base), "truncated code never passes validation");
        }
        auto changed = m;
        for (auto& b : changed.blocks) { if (b.address == base + e.rva) { b.executable = false; } }
        expect(!s::validate_code(changed.reader(), base), "non-executable boundaries are rejected");
    }
    m.put(base + 0x88, k::image_timestamp + 1);
    expect(!s::validate_code(m.reader(), base), "a future game image does not reuse the fixed contract");

    if (image_path) {
        std::ifstream input(std::filesystem::path{image_path}, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        Memory live_image;
        if (!bytes.empty()) { live_image.add(base, bytes.data(), bytes.size(), true); }
        expect(input.is_open() && s::validate_code(live_image.reader(), base), "contracts match the preserved flat runtime module");
    }
}

void test_scope() {
    auto m = fixture();
    auto collection = s::read_collection(m.reader(), base, renderer, outer_stack);
    expect(collection.has_value() && collection->frame_pointer == frame_pointer, "two owning view states and outer stack validate");
    s::CollectionScope<> scope;
    expect(visit(scope, m) == s::Decision::original, "no collection means no interception");
    scope.begin(collection);
    expect(visit(scope, m) == s::Decision::original, "first shadow gathers unchanged");
    for (const auto caller : s::caller_returns) {
        m.put(gather_stack, base + caller);
        expect(visit(scope, m) == s::Decision::duplicate, "same shadow/depth view is deduplicated across every sorted list");
    }
    m.put(shadow + 0x100 + s::depth_view_offset, depth_view);
    expect(visit(scope, m, shadow + 0x100) == s::Decision::original, "different shadows sharing a depth view remain independent");
    m.put(depth_view + 0x100, f::ViewPrefix{base + f::view_vtable_rva, 0, renderer + 0x10, states + 0x100});
    m.put(shadow + s::depth_view_offset, depth_view + 0x100);
    expect(visit(scope, m) == s::Decision::original, "a different depth-view identity is not an exact duplicate");
    m.put(shadow + s::depth_view_offset, depth_view);
    expect(visit(scope, m) == s::Decision::duplicate, "nonadjacent duplicates retain their first identity");
    scope.begin(collection);
    expect(visit(scope, m) == s::Decision::original, "a new collection of the same renderer/frame can gather afresh");
    m.put(renderer + s::frame_offset, uint32_t{19113});
    expect(visit(scope, m) == s::Decision::original, "changed frame never matches a stale collection");
    scope.begin(s::read_collection(m.reader(), base, renderer, outer_stack));
    expect(visit(scope, m) == s::Decision::original && visit(scope, m) == s::Decision::duplicate,
        "next frame gets one complete gather, not cached task data");

    const auto mismatch = [&](auto mutate) {
        auto x = fixture();
        s::CollectionScope<> local;
        local.begin(s::read_collection(x.reader(), base, renderer, outer_stack));
        expect(visit(local, x) == s::Decision::original, "mismatch fixture records its first gather");
        mutate(x);
        expect(visit(local, x) == s::Decision::original, "mismatched/unreadable identity is never suppressed");
    };
    mismatch([](Memory& x) { x.put(gather_stack, base + s::caller_returns[0] + 1); });
    mismatch([](Memory& x) { x.put(renderer + f::views_offset, f::Header{views, 1, 2}); });
    mismatch([](Memory& x) { x.put(renderer + f::views_offset, f::Header{views + 0x100, 2, 2}); });
    mismatch([](Memory& x) { x.put(depth_view, uintptr_t{0}); });
    mismatch([](Memory& x) { x.put(depth_view + 0x10, renderer + 0x110); });
    mismatch([](Memory& x) { x.put(depth_view + 0x18, states + 0x200); });
    mismatch([](Memory& x) { x.put(shadow + s::depth_view_offset, uintptr_t{0}); });
    mismatch([](Memory& x) { x.put(shadow + s::depth_view_offset, uintptr_t{0xFFFFFFFFFFFFFFF8}); });
    mismatch([](Memory& x) { x.put(shadow + s::depth_view_offset, depth_view + 0x1000); });
    mismatch([](Memory& x) { for (auto& b : x.blocks) { if (b.address == gather_stack) { b.bytes.clear(); } } });

    m = fixture(); scope.begin(s::read_collection(m.reader(), base, renderer, outer_stack)); visit(scope, m);
    const auto wrong_scope = [&](uintptr_t r, uintptr_t stack, uintptr_t rbp, uintptr_t reused) {
        return scope.observe(m.reader(), base, r, shadow, stack, rbp, reused) == s::Decision::original;
    };
    expect(wrong_scope(renderer + 0x100, gather_stack, frame_pointer, reused_views) &&
        wrong_scope(renderer, gather_stack + 8, frame_pointer, reused_views) &&
        wrong_scope(renderer, gather_stack, frame_pointer + 8, reused_views) &&
        wrong_scope(renderer, gather_stack, frame_pointer, reused_views + 8),
        "renderer, RSP, RBP and reused-view local must all belong to the observed outer invocation");
    for (const int count : {0, 1, 3, -1}) {
        m.put(renderer + f::views_offset, f::Header{views, count, 4});
        auto mono = s::read_collection(m.reader(), base, renderer, outer_stack);
        expect(!mono, "flat, Synced single-view, and unknown view counts remain untouched");
        scope.begin(mono);
        expect(visit(scope, m) == s::Decision::original, "ineligible outer entry clears the previous scope");
    }
    m = fixture(); m.put(views + f::view_stride + 0x18, states);
    expect(!s::read_collection(m.reader(), base, renderer, outer_stack), "aliased eye state is not a validated two-view collection");
    m = fixture(); m.put(states, uintptr_t{0});
    expect(!s::read_collection(m.reader(), base, renderer, outer_stack), "foreign state vtable is rejected");
    expect(!s::read_collection(m.reader(), base, renderer, 8), "invalid stack cannot underflow the scope arithmetic");
}

void test_limits_and_replay() {
    auto m = fixture();
    const auto c = s::read_collection(m.reader(), base, renderer, outer_stack);
    s::CollectionScope<4, uint8_t> scope;
    scope.begin(c);
    for (size_t i = 0; i < 5; ++i) {
        // All these addresses collide in the small hash table.
        m.put(shadow + i * 0x100 + s::depth_view_offset, depth_view);
        expect(visit(scope, m, shadow + i * 0x100) == (i < 4 ? s::Decision::original : s::Decision::capacity),
            "capacity is bounded without dropping a unique shadow");
    }
    expect(visit(scope, m) == s::Decision::duplicate, "saturation does not evict an in-flight recorded shadow");
    for (size_t i = 0; i < 1024; ++i) {
        scope.begin(c);
        expect(visit(scope, m) == s::Decision::original && visit(scope, m) == s::Decision::duplicate,
            "epoch wrap clears old identities and never drops the next collection's first gather");
    }
    std::atomic_int thread_failures{};
    std::array<std::thread, 4> threads;
    for (auto& thread : threads) {
        thread = std::thread([&] {
            thread_local s::CollectionScope<8> isolated;
            auto local = fixture();
            for (size_t i = 0; i < 128; ++i) {
                isolated.begin(c);
                if (visit(isolated, local) != s::Decision::original || visit(isolated, local) != s::Decision::duplicate) { ++thread_failures; }
            }
        });
    }
    for (auto& thread : threads) { thread.join(); }
    expect(thread_failures == 0, "parallel collection threads do not share dedup state");

    // Replay the observed 19-mesh dispatch followed by a duplicate that grew it
    // to 38 while the first task still held its original setup/static requests.
    for (bool guarded : {false, true}) {
        scope.begin(c);
        size_t meshes{}, dispatches{};
        const auto gather = [&] {
            const auto decision = visit(scope, m);
            if (!guarded || decision != s::Decision::duplicate) { meshes += 19; ++dispatches; }
        };
        gather(); const auto queued_mesh_count = meshes;
        gather();
        expect(meshes == (guarded ? queued_mesh_count : 38) && dispatches == (guarded ? 1 : 2),
            "duplicate producer is stopped before appending or replacing the first task's mesh inputs");
    }
}

void test_activation() {
    struct Hook {
        bool enables{}, disables{}, active{};
        int enable_calls{}, disable_calls{};
        bool enable() { ++enable_calls; return active = enables; }
        bool disable() { ++disable_calls; if (disables) { active = false; } return disables; }
    };
    for (bool first : {false, true}) for (bool second : {false, true}) for (bool rollback : {false, true}) {
        Hook a{first, rollback}, b{second, true};
        const auto result = s::enable_pair(a, b);
        expect((result == s::Activation::ready) == (first && second), "ready requires both entry hooks");
        expect(b.enable_calls == (first ? 1 : 0), "failed first enable cannot enable the inner hook alone");
        expect(a.disable_calls == (first && !second ? 1 : 0), "partial activation rolls back the first hook");
        if (first && !second) {
            expect(result == (rollback ? s::Activation::gather_failed : s::Activation::rollback_failed),
                "rollback failure remains unready and diagnosable");
        }
    }
    safetyhook::Context context{};
    std::memset(&context, 0xA5, sizeof(context));
    auto expected = context;
    expected.rip = base + s::gather_ret_rva;
    s::skip_duplicate(context, base);
    expect(std::memcmp(&context, &expected, sizeof(context)) == 0, "duplicate continuation changes RIP only, not arguments, stack, flags or SIMD");
}

Memory* native_memory{};
s::CollectionScope<> native_scope;
uintptr_t native_base{};
bool native_ready{};
size_t observed{}, skipped{};
void (*clobber)(){};
void native_collection(safetyhook::Context& ctx) {
    if (native_ready) {
        native_scope.begin(s::read_collection(native_memory->reader(), native_base, ctx.rcx, ctx.rsp));
    }
    clobber();
}
void native_gather(safetyhook::Context& ctx) {
    if (native_ready) {
        ++observed;
        native_memory->native_stack = ctx.rsp;
        if (native_scope.observe(native_memory->reader(), native_base, ctx.rdx, ctx.rcx, ctx.rsp, ctx.rbp, ctx.r9) == s::Decision::duplicate) {
            ++skipped;
            s::skip_duplicate(ctx, native_base);
        }
    }
    clobber();
}

void test_native_hooks(bool exhausted_near_space) {
    const auto reservation_size = exhausted_near_space ? 0x100040000ull : 0x10000ull;
    auto* reservation = static_cast<uint8_t*>(VirtualAlloc(nullptr, reservation_size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reservation != nullptr, "shadow trampoline fixture reserves address space");
    if (!reservation) { return; }
    auto* page = static_cast<uint8_t*>(VirtualAlloc(reservation + (exhausted_near_space ? 0x80000000ull : 0), 0x4000, MEM_COMMIT, PAGE_READWRITE));
    if (!page) { expect(false, "shadow fixture commits four pages"); VirtualFree(reservation, 0, MEM_RELEASE); return; }
    std::memset(page, 0x90, 0x4000);
    native_base = reinterpret_cast<uintptr_t>(page) - 0x7A2000;
    auto* outer = page + 0xDCC;
    auto* inner = page + 0x22F8;
    auto* inner_epilogue = page + 0x23E1;
    std::memcpy(outer, s::collect_code.data(), s::collect_code.size());
    std::memcpy(inner, s::gather_code.data(), 14);
    std::memcpy(inner_epilogue, s::gather_epilogue.data() + 5, s::gather_epilogue.size() - 5);
    const auto jump = [](uint8_t* at, uint8_t* target, uint8_t opcode = 0xE9) {
        *at = opcode; const auto disp = static_cast<int32_t>(target - at - 5); std::memcpy(at + 1, &disp, 4);
    };
    const uint8_t setup[]{0x48,0x8B,0xD9,0x48,0x8B,0xF2,0x41,0xBC,3,0,0,0,
        0x49,0xBA,1,2,3,4,5,6,7,8,0x49,0xBB,8,7,6,5,4,3,2,1,0x66,0x0F,0x6E,0xE9};
    std::memcpy(outer + s::collect_code.size(), setup, sizeof(setup));
    jump(outer + s::collect_code.size() + sizeof(setup), page + 0xF47);
    const uint8_t args[]{0x48,0x8B,0xCE,0x48,0x8B,0xD3,0x4C,0x8D,0x4D,0xC0};
    std::memcpy(page + 0xF47, args, sizeof(args));
    jump(page + 0xF6A, inner, 0xE8); // Original owned return RVA.
    const uint8_t repeat[]{0x41,0xFF,0xCC,0x75,0xD3}; // dec r12d; jnz F47
    std::memcpy(page + 0xF6F, repeat, sizeof(repeat));
    const uint8_t output[]{0x48,0x8B,0x45,0x50,0x4C,0x89,0x10,0x4C,0x89,0x58,0x08,
        0xF3,0x0F,0x7F,0x68,0x10,0x48,0x89,0x50,0x20,0x4C,0x89,0x40,0x28};
    std::memcpy(page + 0xF74, output, sizeof(output));
    const uint8_t epilogue[]{0x48,0x8B,0x9C,0x24,0xD8,0,0,0,0x48,0x81,0xC4,0x80,0,0,0,
        0x41,0x5F,0x41,0x5E,0x41,0x5D,0x41,0x5C,0x5F,0x5E,0x5D,0xC3};
    std::memcpy(page + 0xF74 + sizeof(output), epilogue, sizeof(epilogue));
    inner[14] = 0xFF; inner[15] = 0x01; // inc dword [rcx]: stand-in for the original gather.
    jump(inner + 16, inner_epilogue);
    const uint8_t destroy_volatiles[]{0x31,0xC9,0x31,0xD2,0x45,0x31,0xC0,0x45,0x31,0xC9,
        0x45,0x31,0xD2,0x45,0x31,0xDB,0x66,0x0F,0xEF,0xED,0xC3};
    std::memcpy(page + 0x3000, destroy_volatiles, sizeof(destroy_volatiles));
    clobber = reinterpret_cast<void(*)()>(page + 0x3000);
    DWORD old{};
    expect(VirtualProtect(page, 0x4000, PAGE_EXECUTE_READ, &old) != 0, "shadow fixture has read-only executable pages");
    FlushInstructionCache(GetCurrentProcess(), page, 0x4000);
    uint64_t counter{};
    std::array<uint64_t, 6> output_values{};
    using Call = void(*)(uintptr_t, uint64_t*, uint64_t*, uintptr_t);
    const auto call = reinterpret_cast<Call>(outer);
    auto memory = fixture(native_base, reinterpret_cast<uintptr_t>(&counter));
    native_memory = &memory;
    call(renderer, &counter, output_values.data(), 0);
    expect(counter == 3, "unhooked fixture runs all three original gathers");
    {
        auto hooks = s::prepare_hooks(native_base, native_collection, native_gather,
            [](void* target, safetyhook::MidHookFn callback, safetyhook::MidHook::Flags flags) {
                return safetyhook::MidHook::create(safetyhook::Allocator::create(), target, callback, flags);
            });
        expect(hooks.collection && hooks.gather && !hooks.error, "both original prologues relocate even without near address space");
        if (hooks.collection && hooks.gather) {
            if (exhausted_near_space) { expect(hooks.collection.original_bytes().size() >= 14 && hooks.gather.original_bytes().size() == 14,
                "forced far hooks stop at complete, relocation-safe instruction boundaries"); }
            expect(s::enable_pair(hooks.collection, hooks.gather) == s::Activation::ready, "both native entry hooks enable together");
            for (const bool ready : {false, true}) {
                native_ready = ready;
                for (size_t repeat_index = 0; repeat_index < 4; ++repeat_index) {
                    counter = observed = skipped = 0; output_values = {};
                    call(renderer, &counter, output_values.data(), 0);
                    expect(counter == (ready ? 1 : 3) && skipped == (ready ? 2 : 0) && observed == (ready ? 3 : 0),
                        "native entry guard skips only duplicates and resets on each outer invocation");
                    expect(output_values[0] == 0x0807060504030201 && output_values[1] == 0x0102030405060708 &&
                        output_values[2] == renderer && output_values[3] == 0 && output_values[4] == renderer &&
                        output_values[5] == reinterpret_cast<uintptr_t>(output_values.data()),
                        "volatile GP/SIMD registers survive both midhooks and the bare-RET skip");
                }
            }
            memory.put(renderer + f::views_offset, f::Header{views, 1, 2});
            counter = 0; call(renderer, &counter, output_values.data(), 0);
            expect(counter == 3, "native trampoline leaves mono/Synced original calls untouched");
            native_ready = false;
            expect(hooks.gather.disable().has_value() && hooks.collection.disable().has_value(), "both shadow hooks remove cleanly");
            counter = 0; call(renderer, &counter, output_values.data(), 0);
            expect(counter == 3, "unhook restores all original gathers");
        }
    }
    MEMORY_BASIC_INFORMATION info{};
    expect(VirtualQuery(page, &info, sizeof(info)) && info.Protect == PAGE_EXECUTE_READ,
        "shadow hook installation/removal preserves page protection");
    native_memory = nullptr;
    VirtualFree(reservation, 0, MEM_RELEASE);
}
}

int test_ktjl_shadow_gather(const wchar_t* image_path) {
    test_contracts(image_path);
    test_scope();
    test_limits_and_replay();
    test_activation();
    test_native_hooks(false);
    test_native_hooks(true);
    std::cout << "KTJL shadow-gather tests: " << failures << " failures\n";
    return failures;
}
