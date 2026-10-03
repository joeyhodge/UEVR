#define NOMINMAX
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <iostream>
#include "mods/vr/KtjLCloudHook.hpp"

namespace {
namespace c = uevr::ktjl::cloud;
int failures{};
c::Boundary boundary{};
uintptr_t image_base{};
bool reject_import{};
size_t callbacks{};
void (*clobber_registers)(){};

void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}

void consumer(safetyhook::Context& ctx) {
    if (!c::consumer_enabled(ctx, boundary)) { return; }
    ++callbacks;
    clobber_registers();
    if (reject_import) { c::skip_unsafe_import(ctx, boundary, image_base); }
}

void test_selection() {
    using M = safetyhook::MidHook;
    using I = safetyhook::InlineHook;
    constexpr uintptr_t base = 0x140000000;
    const auto branch = reinterpret_cast<uint8_t*>(base + c::hook_rva + 2);
    const auto callback = +[](safetyhook::Context&) {};
    const std::array errors{
        M::Error::bad_allocation(safetyhook::Allocator::Error::BAD_VIRTUAL_ALLOC),
        M::Error::bad_inline_hook(I::Error::bad_allocation(safetyhook::Allocator::Error::NO_MEMORY_IN_RANGE)),
        M::Error::bad_inline_hook(I::Error::failed_to_decode_instruction(branch)),
        M::Error::bad_inline_hook(I::Error::failed_to_unprotect(branch)),
        M::Error::bad_inline_hook(I::Error::unsupported_instruction_in_trampoline(branch)),
        M::Error::bad_inline_hook(I::Error::ip_relative_instruction_out_of_range(branch + 1)),
        M::Error::bad_inline_hook(I::Error::ip_relative_instruction_out_of_range(branch)),
    };
    for (size_t i = 0; i < errors.size(); ++i) {
        size_t calls{};
        const auto prepared = c::prepare_consumer_hook(base, callback,
            [&](void* target, safetyhook::MidHookFn fn, M::Flags flags) -> std::expected<M, M::Error> {
                ++calls;
                expect(target == reinterpret_cast<void*>(base + (calls == 1 ? c::hook_rva : c::fallback_hook_rva)),
                    "only the two exact consumer boundaries may be attempted");
                expect(fn == callback && flags == M::StartDisabled, "prepare never enables a hook before publishing its context");
                return std::unexpected{errors[i]};
            });
        expect(calls == (i == errors.size() - 1 ? 2 : 1), "fallback is limited to the exact unrelocatable JZ error");
        expect(!prepared.hook && prepared.error && prepared.primary_error, "both failed attempts retain their error and no hook");
    }
    safetyhook::Context ctx{};
    ctx.rax = 0x123456789ABCDE01;
    ctx.rip = 0x1234;
    c::skip_unsafe_import(ctx, c::Boundary::predicate, base);
    expect(ctx.rax == 0x123456789ABCDE00 && ctx.rip == 0x1234, "original rejection clears only AL");
    ctx.rsi = 0x6789;
    c::skip_unsafe_import(ctx, c::Boundary::resource_import, base);
    expect(ctx.rip == base + c::fallback_skip_rva && ctx.rsi == 0x6789 && ctx.rax == 0x123456789ABCDE00,
        "fallback rejection redirects only RIP to the original loop-index restore");
}

void test_execution(bool exhausted_near_space) {
    // Reserve address space only: just one 4-KB page is committed. The large
    // reservation deterministically removes the entire rel32 allocation window.
    const auto reserve_size = exhausted_near_space ? 0x100020000ull : 0x10000ull;
    auto* reservation = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reservation != nullptr, "cloud trampoline fixture reserves address space");
    if (!reservation) { return; }
    auto* page = static_cast<uint8_t*>(VirtualAlloc(reservation + (exhausted_near_space ? 0x80000000ull : 0),
        0x1000, MEM_COMMIT, PAGE_READWRITE));
    expect(page != nullptr, "cloud trampoline fixture commits one page");
    if (!page) { VirtualFree(reservation, 0, MEM_RELEASE); return; }
    std::memset(page, 0x90, 0x1000); // No executable INT3 padding for an allocator fallback.
    image_base = reinterpret_cast<uintptr_t>(page) - (c::hook_rva & ~uintptr_t{0xFFF});
    auto* entry = page + (c::hook_rva & 0xFFF);
    auto* fallback = page + (c::fallback_hook_rva & 0xFFF);
    auto* restore = page + (c::fallback_skip_rva & 0xFFF);
    static_assert(c::fallback_hook_rva - c::consumer_rva == 27);
    std::memcpy(entry, c::consumer_code.data() + (c::hook_rva - c::consumer_rva), 8);
    std::memcpy(fallback, c::consumer_code.data() + (c::fallback_hook_rva - c::consumer_rva), 21);
    std::memcpy(restore, c::fallback_skip_code.data(), 3);

    // A minimal caller preserves Win64 nonvolatiles and supplies the same RBP
    // frame slots consumed by KTJL's three register loads and RSI restoration.
    const uint8_t prologue[]{0x55,0x56,0x41,0x54,0x41,0x57,0x48,0x8B,0xEA,
        0xBE,0x78,0x56,0x34,0x12,0x89,0x75,0x90,
        0x41,0xBF,0x11,0x22,0x33,0x44,0x41,0xBC,0x22,0x33,0x44,0x55,
        0x41,0xBB,0x33,0x44,0x55,0x66,0xF3,0x0F,0x6F,0x6D,0x40,0x48,0x8B,0xC1};
    std::memcpy(page, prologue, sizeof(prologue));
    auto* jump = page + sizeof(prologue);
    jump[0] = 0xE9;
    const auto displacement = static_cast<int32_t>(entry - (jump + 5));
    std::memcpy(jump + 1, &displacement, 4);
    // Simulate the intervening RSI overwrite and a non-Boolean RAX. At the
    // fallback the predicate has already passed, so AL must not be re-tested.
    const uint8_t gap[]{0x48,0xC7,0xC6,0x68,0x24,0x57,0x13,0x48,0xC7,0xC0,0,0,0,0};
    std::memcpy(entry + 8, gap, sizeof(gap));
    const uint8_t output[]{0x49,0x89,0x30,0x4D,0x89,0x78,0x08,0x4D,0x89,0x60,0x10,0x4D,0x89,0x58,0x18,
        0xF3,0x41,0x0F,0x7F,0x68,0x20,0x4D,0x89,0x48,0x30};
    const uint8_t epilogue[]{0x41,0x5F,0x41,0x5C,0x5E,0x5D,0xC3};
    const auto finish = [&](uint8_t* at, uint32_t value) {
        std::memcpy(at, output, sizeof(output)); at += sizeof(output);
        *at++ = 0xB8; std::memcpy(at, &value, 4); at += 4;
        std::memcpy(at, epilogue, sizeof(epilogue));
    };
    finish(fallback + 21, 7);
    finish(restore + 3, 3);
    const uint8_t clobber[]{0x31,0xC9,0x31,0xD2,0x45,0x31,0xC0,0x45,0x31,0xC9,
        0x45,0x31,0xD2,0x45,0x31,0xDB,0x66,0x0F,0xEF,0xED,0xC3};
    std::memcpy(page + 0xB00, clobber, sizeof(clobber));
    clobber_registers = reinterpret_cast<void(*)()>(page + 0xB00);
    DWORD old{};
    expect(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old) != 0, "fixture becomes executable and read-only");
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    using Call = uint64_t(*)(uint64_t, void*, uint64_t*, uint64_t);
    const auto call = reinterpret_cast<Call>(page);
    std::array<uint8_t, 0x600> frame{};
    auto* rbp = frame.data() + 0x100;
    const std::array<uint64_t, 3> loads{0x1234567890ABCDEF,0xFEDCBA0987654321,0x1379BDF02468ACE0};
    for (size_t i = 0; i < loads.size(); ++i) {
        std::memcpy(rbp + std::array<size_t,3>{0x490,0x4BC,0x4C8}[i], &loads[i], 8);
    }
    const std::array<uint64_t, 2> vector{0x123456789ABCDEF0,0xFEDCBA9876543210};
    std::memcpy(rbp + 0x40, vector.data(), sizeof(vector));
    std::array<uint64_t, 7> out{};
    expect(call(1, rbp, out.data(), 0xF00DF00D) == 7 && call(0, rbp, out.data(), 0xF00DF00D) == 3,
        "unhooked fixture has the original predicate outcomes");
    {
        auto prepared = c::prepare_consumer_hook(image_base, consumer,
            [](void* target, safetyhook::MidHookFn fn, safetyhook::MidHook::Flags flags) {
                return safetyhook::MidHook::create(safetyhook::Allocator::create(), target, fn, flags);
            });
        expect(bool(prepared.hook), "cloud guard prepares even without rel32 trampoline space");
        expect(prepared.boundary == (exhausted_near_space ? c::Boundary::resource_import : c::Boundary::predicate),
            "successful original placement is retained; constrained placement uses only the exact fallback");
        if (exhausted_near_space) {
            expect(prepared.primary_error && !prepared.error && prepared.hook.original_bytes().size() == 14,
                "near-space exhaustion reproduces original failure and uses a far absolute trampoline");
        }
        if (prepared.hook) {
            boundary = prepared.boundary;
            expect(prepared.hook.enable().has_value(), "prepared cloud guard enables");
            for (int repeat = 0; repeat < 3; ++repeat) {
                for (bool reject : {false, true}) {
                    reject_import = reject;
                    for (uint64_t predicate : {0ull,1ull,2ull,0x100ull,0x101ull}) {
                        callbacks = 0; out = {};
                        const auto result = call(predicate, rbp, out.data(), 0xF00DF00D);
                        const bool active = (predicate & 0xFF) != 0;
                        const bool accepted = active && !reject;
                        expect(result == (accepted ? 7 : 3) && callbacks == (active ? 1 : 0),
                            "both boundaries preserve disabled, allowed and rejected import control flow");
                        expect(out[0] == (accepted ? 0x13572468 : 0x12345678), "rejection restores the original loop index, never a state pointer");
                        expect(out[1] == (accepted ? loads[0] : 0x44332211) && out[2] == (accepted ? loads[1] : 0x55443322) &&
                            out[3] == (accepted ? loads[2] : 0x66554433), "relocated loads and original nonvolatiles survive both continuations");
                        expect(out[4] == vector[0] && out[5] == vector[1] && out[6] == 0xF00DF00D,
                            "allocator-like volatile and XMM clobbers do not escape the midhook");
                    }
                }
            }
            expect(prepared.hook.disable().has_value(), "consumer guard removes cleanly");
            expect(call(1, rbp, out.data(), 0xF00DF00D) == 7 && call(0, rbp, out.data(), 0xF00DF00D) == 3,
                "removal restores both original paths");
            MEMORY_BASIC_INFORMATION info{};
            expect(VirtualQuery(page, &info, sizeof(info)) && info.Protect == PAGE_EXECUTE_READ,
                "installation and removal preserve original page protection");
        }
    }
    VirtualFree(reservation, 0, MEM_RELEASE);
}
}

int test_ktjl_cloud_hook_installation() {
    test_selection();
    test_execution(false);
    test_execution(true);
    std::cout << "KTJL cloud-hook installation: " << failures << " failures\n";
    return failures;
}
