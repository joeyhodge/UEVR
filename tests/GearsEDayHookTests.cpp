#define NOMINMAX
#include <Windows.h>
#include <safetyhook.hpp>
#include <safetyhook/allocator.hpp>
#include <array>
#include <cstring>
#include <iostream>
#include <string_view>
#include "sdk/GearsEDayDiscovery.hpp"

namespace {
int failures{};
void expect(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
SafetyHookInline* active_hook{};
bool redirect_ui{};
uintptr_t ui_texture{};
int callback_calls{};
int32_t index_before{-1};
void free_observer(void* array, void* object) {
    ++callback_calls;
    std::memcpy(&index_before, static_cast<uint8_t*>(object) + 12, 4);
    active_hook->unsafe_call<void>(array, object);
}
void* register_observer(void* builder, void* texture, const wchar_t* name, uint8_t flags) {
    ++callback_calls;
    if (redirect_ui && std::wstring_view{name} == L"SlateOutputTexture") {
        texture = reinterpret_cast<void*>(ui_texture);
    }
    return active_hook->unsafe_call<void*>(builder, texture, name, flags);
}
void* view_observer(void* view, void* options) {
    ++callback_calls;
    return active_hook->unsafe_call<void*>(view, options);
}
void* unused_observer(void*, void*) { return nullptr; }

bool private_trampoline(const SafetyHookInline& hook) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(hook.trampoline().data(), &region, sizeof(region)) == sizeof(region) && region.Type == MEM_PRIVATE;
}
}

int test_goweday_hook_boundaries() {
    failures = 0;
    // Reproduce the game's observed address-space condition in this test process
    // only. Reserve virtual address space, not 4 GiB of physical/committed memory.
    constexpr size_t reserve_size = 0x100020000ULL;
    auto* reserve = static_cast<uint8_t*>(VirtualAlloc(nullptr, reserve_size, MEM_RESERVE, PAGE_NOACCESS));
    expect(reserve != nullptr, "Gears far-hook address-space fixture reserves");
    if (!reserve) { return failures; }
    auto* page = reserve + 0x80010000ULL;
    const bool committed = VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE) == page;
    expect(committed, "Gears fixture commits only its code page");
    if (!committed) { VirtualFree(reserve, 0, MEM_RELEASE); return failures; }
    std::memset(page, 0xcc, 0x1000);

    namespace g = sdk::gears_eday;
    // Original entry bytes, followed by small test-owned ABI consumers. No game
    // code is executed: the source contracts themselves have separate tests.
    std::memcpy(page, g::object_free_code.data(), 18);
    const uint8_t free_tail[]{0x48,0x89,0x31, 0xc7,0x42,0x0c,0xff,0xff,0xff,0xff,
        0x48,0x83,0xc4,0x38,0x5f,0x5e,0x5d,0x5b,0xc3};
    std::memcpy(page + 18, free_tail, sizeof(free_tail));

    auto* registration = page + 0x100;
    std::memcpy(registration, g::slate_register_entry_code.data(), 21);
    const uint8_t register_tail[]{0x48,0x89,0x1f,0x48,0x89,0x6f,0x08,0x40,0x88,0x77,0x10,
        0x48,0x8b,0xc3,0x48,0x83,0xc4,0x38,0x5f,0x5e,0x5d,0x5b,0xc3};
    std::memcpy(registration + 21, register_tail, sizeof(register_tail));

    auto* constructor = page + 0x200;
    constexpr uint8_t view_entry[]{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57};
    std::memcpy(constructor, view_entry, sizeof(view_entry));
    const uint8_t view_tail[]{0x48,0x8b,0x82,0x58,0x01,0,0,0x48,0x89,0x41,0x08,
        0x48,0x8b,0x82,0x60,0x01,0,0,0x48,0x89,0x41,0x10,0x48,0x8b,0xc1,
        0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5d,0x48,0x8b,0x5c,0x24,0x18,0xc3};
    std::memcpy(constructor + sizeof(view_entry), view_tail, sizeof(view_tail));

    auto* short_destructor = page + 0x300;
    constexpr uint8_t short_entry[]{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8d,0x05,0,0,0,0,0x48,0x8b,0xd9};
    std::memcpy(short_destructor, short_entry, sizeof(short_entry));

    auto* draw_window = page + 0x400;
    constexpr uint8_t draw_entry[]{0x40,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
        0x48,0x81,0xec,0x60,0x03,0,0,0x48,0x8b,0x05,0,0,0,0,0x48,0x33,0xc4,
        0x48,0x89,0x84,0x24,0xb0,0x02,0,0};
    std::memset(draw_window, 0x90, 0x100);
    std::memcpy(draw_window, draw_entry, sizeof(draw_entry));
    std::memcpy(draw_window + 0x25, g::slate_entry_code.data(), g::slate_entry_code.size());
    std::memcpy(draw_window + 0x80, g::slate_register_code.data(), g::slate_register_code.size());
    auto* scan_registration = page + 0x800;
    std::memcpy(scan_registration, g::slate_register_entry_code.data(), g::slate_register_entry_code.size());
    auto* name = page + 0xe00;
    constexpr char16_t output_name[] = u"SlateOutputTexture";
    std::memcpy(name, output_name, sizeof(output_name));
    const auto name_disp = static_cast<int32_t>(name - (draw_window + 0x87));
    const auto call_disp = static_cast<int32_t>(scan_registration - (draw_window + 0x92));
    std::memcpy(draw_window + 0x83, &name_disp, 4);
    draw_window[0x8d] = 0xe8;
    std::memcpy(draw_window + 0x8e, &call_disp, 4);
    draw_window[0x92] = 0xc3;
    std::array<uint8_t, 0x1000> original{};
    std::memcpy(original.data(), page, original.size());
    DWORD old{};
    expect(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READ, &old) != FALSE, "Gears fixture code starts read/execute");
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);

    auto allocator = safetyhook::Allocator::create_private();
    const auto nearby = allocator->allocate_near({page}, 64);
    expect(!nearby, "Gears fixture really has no rel32 allocation space");
    auto rejected = SafetyHookInline::create(allocator, short_destructor, unused_observer, SafetyHookInline::StartDisabled);
    expect(!rejected && rejected.error().type == SafetyHookInline::Error::IP_RELATIVE_INSTRUCTION_OUT_OF_RANGE,
        "old short destructor cannot be far-relocated; failure leaves its bytes unchanged");
    expect(std::memcmp(short_destructor, short_entry, sizeof(short_entry)) == 0, "rejected destructor never writes the entry");

    const auto fixture_memory = sdk::discovery::Memory{page,
        [](void* ctx, uintptr_t address, void* output, size_t size) {
            const auto begin = reinterpret_cast<uintptr_t>(ctx);
            if (address < begin || address - begin > 0x1000 || size > 0x1000 - (address - begin)) { return false; }
            std::memcpy(output, reinterpret_cast<void*>(address), size);
            return true;
        },
        [](void* ctx, uintptr_t address, size_t size) {
            const auto begin = reinterpret_cast<uintptr_t>(ctx);
            return address >= begin && address - begin <= 0x1000 && size <= 0x1000 - (address - begin);
        }};
    auto draw = SafetyHookInline::create(allocator, draw_window, unused_observer, SafetyHookInline::StartDisabled);
    expect(draw.has_value(), "original Slate entry supports the same far-hook layout as the game");
    if (draw) {
        expect(private_trampoline(*draw) && draw->enable().has_value(), "Slate DrawWindow far entry is actually patched before discovery");
        expect(draw_window[0] == 0xff && draw_window[1] == 0x25, "Slate test covers the absolute-jump form, not just a rel32 hook");
        const auto found = g::slate_output_register(fixture_memory, reinterpret_cast<uintptr_t>(draw_window), draw->original_bytes().size());
        expect(found && found->name_reference == reinterpret_cast<uintptr_t>(draw_window + 0x80) &&
            found->callsite == reinterpret_cast<uintptr_t>(draw_window + 0x8d) &&
            found->target == reinterpret_cast<uintptr_t>(scan_registration),
            "production Slate scan finds the exact output registration with the real far hook already enabled");
        expect(draw->disable().has_value(), "Slate DrawWindow hook disables after discovery");
        draw->reset();
    }

    std::array<uint8_t, 32> object{};
    uint64_t retired_index{};
    int32_t index = 73;
    std::memcpy(object.data() + 12, &index, sizeof(index));
    auto free = SafetyHookInline::create(allocator, page, free_observer, SafetyHookInline::StartDisabled);
    expect(free.has_value(), "validated free-index entry supports a far hook without an image cave");
    if (free) {
        active_hook = &*free;
        callback_calls = 0;
        expect(private_trampoline(*free) && free->enable().has_value(), "free-index trampoline is private and enables after publication");
        reinterpret_cast<void(*)(void*, void*)>(page)(&retired_index, object.data());
        int32_t after{}; std::memcpy(&after, object.data() + 12, 4);
        expect(callback_calls == 1 && index_before == 73 && retired_index == 73 && after == -1,
            "free-index observer runs before invalidation, calls the original exactly once, and preserves arguments");
        expect(free->disable().has_value(), "free-index far hook disables");
    }

    auto ui = SafetyHookInline::create(allocator, registration, register_observer, SafetyHookInline::StartDisabled);
    expect(ui.has_value(), "validated UI registration entry supports a far hook");
    if (ui) {
        active_hook = &*ui;
        expect(private_trampoline(*ui) && ui->enable().has_value(), "UI entry publishes a private trampoline");
        constexpr uintptr_t source = 0x1234567812345678ULL;
        ui_texture = 0x2345678923456789ULL;
        using Register = void*(*)(void*, void*, const wchar_t*, uint8_t);
        const auto call = reinterpret_cast<Register>(registration);
        for (const auto* name : {L"SlateOutputTexture", L"StereoSpectatorSwapChainTexture", L"OtherTexture"}) {
            for (const bool enabled : {false, true}) {
                callback_calls = 0; redirect_ui = enabled;
                std::array<uint64_t, 3> inputs{};
                const auto expected = enabled && std::wstring_view{name} == L"SlateOutputTexture" ? ui_texture : source;
                const auto result = call(inputs.data(), reinterpret_cast<void*>(source), name, 0xa5);
                expect(callback_calls == 1 && reinterpret_cast<uintptr_t>(result) == expected && inputs[0] == expected &&
                    inputs[1] == reinterpret_cast<uintptr_t>(name) && inputs[2] == 0xa5,
                    "UI far hook retains builder/name/flags/64-bit return and changes only the opted-in Slate texture");
            }
        }
        expect(ui->disable().has_value(), "UI far hook disables");
    }

    auto view = SafetyHookInline::create(allocator, constructor, view_observer, SafetyHookInline::StartDisabled);
    expect(view.has_value(), "actual view constructor prologue supports a far hook");
    if (view) {
        active_hook = &*view;
        expect(private_trampoline(*view) && view->enable().has_value(), "view constructor trampoline is private");
        std::array<uintptr_t, 0x200 / 8> options{};
        options[0x158 / 8] = 0x1234567812345678ULL;
        options[0x160 / 8] = 0x2345678923456789ULL;
        std::array<uintptr_t, 4> output{};
        callback_calls = 0;
        auto* result = reinterpret_cast<void*(*)(void*, void*)>(constructor)(output.data(), options.data());
        expect(result == output.data() && output[1] == options[0x158 / 8] && output[2] == options[0x160 / 8] && callback_calls == 1,
            "view far trampoline preserves this/InitOptions and exact family/state copies");
        expect(view->disable().has_value(), "view constructor far hook disables");
    }
    active_hook = nullptr;
    if (view) { view->reset(); }
    if (ui) { ui->reset(); }
    if (free) { free->reset(); }
    expect(std::memcmp(original.data(), page, original.size()) == 0, "Gears hook teardown restores every test code byte");
    VirtualFree(reserve, 0, MEM_RELEASE);
    return failures;
}
