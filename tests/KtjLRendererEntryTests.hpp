#pragma once

#include "mods/vr/KtjLRendererEntry.hpp"

sdk::detail::FamilyLayout ktjl_renderer_test_layout() {
    sdk::detail::FamilyLayout layout{};
    layout.has_vtable = false;
    layout.views = 0;
    layout.render_target = 0x18;
    layout.scene_interface = 0x20;
    layout.frame_count = 0x40;
    return layout;
}

void test_ktjl_renderer_entry() {
    namespace r = uevr::ktjl::renderer;
    namespace k = sdk::ktjl;
    for (const bool ue425 : {false, true}) {
        for (const bool dx12 : {false, true}) {
            for (const bool native_fix : {false, true}) {
                expect(r::should_use_native_fix(L"D:\\Games\\SuicideSquad_KTJL.exe", ue425, dx12, native_fix) ==
                    (ue425 && dx12 && native_fix), "KTJL renderer repair requires UE4.25, DX12 and effective Native Fix");
            }
        }
    }
    for (const auto path : {L"SHCO-Win64-Shipping.exe", L"Medium-Win64-Shipping.exe", L"HellbladeGame-Win64-Shipping.exe",
                           L"Sifu-Win64-Shipping.exe", L"Other.exe", L"SuicideSquad_KTJL.exe.bak",
                           L"D:\\SuicideSquad_KTJL.exe\\Other.exe", L""}) {
        expect(!r::should_use_native_fix(path, true, true, true), "other titles and path lookalikes retain their renderer path");
    }
    expect(r::should_use_native_fix(L"D:/Games/SUICIDESQUAD_KTJL.EXE", true, true, true), "exact executable matching is case insensitive");
    for (int mode : {0, 1, 2}) {
        for (bool requested : {false, true}) {
            for (bool afr : {false, true}) {
                const bool effective_native_fix = requested && mode == 0 && !afr;
                expect(r::should_use_native_fix(L"SuicideSquad_KTJL.exe", true, true, effective_native_fix) == effective_native_fix,
                    "ordinary Native, Synced/Ghost and other modes do not opt into the new resolver");
            }
        }
    }

    struct Fixture {
        struct Block { uintptr_t address; std::vector<uint8_t> bytes; bool executable; };
        std::vector<Block> blocks;
        void add(uintptr_t address, std::span<const uint8_t> bytes, bool code = false) {
            blocks.push_back({address, {bytes.begin(), bytes.end()}, code});
        }
        sdk::discovery::Memory memory() {
            return {this, [](void* context, uintptr_t address, void* output, size_t size) {
                for (const auto& block : static_cast<Fixture*>(context)->blocks) {
                    if (address >= block.address && address - block.address <= block.bytes.size() &&
                        size <= block.bytes.size() - (address - block.address)) {
                        std::memcpy(output, block.bytes.data() + address - block.address, size); return true;
                    }
                }
                return false;
            }, [](void* context, uintptr_t address, size_t size) {
                for (const auto& block : static_cast<Fixture*>(context)->blocks) {
                    if (block.executable && address >= block.address && address - block.address <= block.bytes.size() &&
                        size <= block.bytes.size() - (address - block.address)) { return true; }
                }
                return false;
            }};
        }
    };
    for (const uintptr_t base : {uintptr_t{0x140000000}, uintptr_t{0x7FF600000000}}) {
        Fixture original;
        std::array<uint8_t, 512> pe{};
        const auto put = [&](size_t offset, auto value) { std::memcpy(pe.data() + offset, &value, sizeof(value)); };
        put(0, uint16_t{0x5A4D}); put(0x3C, uint32_t{0x80}); put(0x80, uint32_t{0x4550});
        put(0x84, uint16_t{0x8664}); put(0x88, k::image_timestamp); put(0x98, uint16_t{0x20B}); put(0xD0, k::image_size);
        original.add(base, pe);
        original.add(base + 0xAFFF76, k::class_name_access, true);
        original.add(base + 0xA1DC2F, k::object_iteration, true);
        original.add(base + 0x58DC4E, k::name_entry_access, true);
        std::array<uint8_t, r::end_rva - r::entry_rva> code{};
        const auto copy = [&](uint32_t rva, const auto& bytes) {
            std::copy(bytes.begin(), bytes.end(), code.begin() + (rva - r::entry_rva));
        };
        copy(r::entry_rva, r::entry_code); copy(r::frame_loop_rva, r::frame_loop_code);
        copy(r::create_rva, r::create_code); copy(r::return_rva, r::return_code);
        original.add(base + r::entry_rva, code, true);
        original.add(base + r::unwind_rva, r::root_unwind);
        const uintptr_t method = base + r::entry_rva;
        original.add(base + r::method_table_slot_rva,
            {reinterpret_cast<const uint8_t*>(&method), sizeof(method)});
        const r::RuntimeEntry entry{base, r::entry_rva, r::end_rva, r::unwind_rva};
        const auto layout = ktjl_renderer_test_layout();
        const auto resolve = [&](Fixture& fixture) {
            return r::resolve(fixture.memory(), base, base + r::callback_return_rva, entry, layout);
        };
        expect(resolve(original) == base + r::entry_rva, "exact callable KTJL renderer validates at preferred and relocated bases");

        const auto damage = [&](size_t block, size_t offset) {
            auto changed = original; changed.blocks[block].bytes[offset] ^= 1;
            expect(!resolve(changed), "mutated image, ABI, frame provenance, callback or unwind fails closed");
        };
        for (const auto offset : {0, 0x3C, 0x80, 0x84, 0x88, 0x98, 0xD0}) { damage(0, offset); }
        for (size_t block : {1, 2, 3, 5, 6}) {
            for (size_t i = 0; i < original.blocks[block].bytes.size(); ++i) { damage(block, i); }
        }
        for (const auto [rva, count] : {std::pair{r::entry_rva, r::entry_code.size()},
                std::pair{r::frame_loop_rva, r::frame_loop_code.size()}, std::pair{r::create_rva, r::create_code.size()},
                std::pair{r::return_rva, r::return_code.size()}}) {
            for (size_t i = 0; i < count; ++i) { damage(4, rva - r::entry_rva + i); }
        }
        for (size_t block = 1; block < original.blocks.size(); ++block) {
            auto changed = original; changed.blocks[block].bytes.pop_back();
            expect(!resolve(changed), "truncated image/code/unwind evidence fails closed");
            if (original.blocks[block].executable) {
                changed = original; changed.blocks[block].executable = false;
                expect(!resolve(changed), "non-executable code is never hooked");
            }
        }
        for (const auto callback : {uintptr_t{}, base + r::entry_rva, base + r::callback_return_rva - 1,
                                   base + r::callback_return_rva + 1, base + r::end_rva}) {
            expect(!r::resolve(original.memory(), base, callback, entry, layout), "only the proven extension return address selects the renderer");
        }
        for (int field = 0; field < 4; ++field) {
            auto changed = entry;
            switch (field) {
            case 0: changed.image_base += 0x1000; break;
            case 1: ++changed.begin; break;
            case 2: --changed.end; break;
            case 3: ++changed.unwind; break;
            }
            expect(!r::resolve(original.memory(), base, base + r::callback_return_rva, changed, layout),
                "a different image, continuation, function extent or unwind record cannot become a callable entry");
        }
        for (const uint32_t offset : {0, 0x3C, 0x44, 0x48, 0x5C, 0xBC}) {
            auto changed = layout; changed.frame_count = offset;
            expect(!r::resolve(original.memory(), base, base + r::callback_return_rva, entry, changed),
                "stock/other-game frame offsets are never borrowed or overwritten");
        }
        for (int field = 0; field < 5; ++field) {
            auto changed = layout;
            switch (field) {
            case 0: changed.has_vtable.reset(); break;
            case 1: changed.views.reset(); break;
            case 2: changed.render_target.reset(); break;
            case 3: changed.scene_interface.reset(); break;
            case 4: changed.frame_count.reset(); break;
            }
            expect(!r::resolve(original.memory(), base, base + r::callback_return_rva, entry, changed), "incomplete independent layout discovery must retry");
        }
        auto wrong = layout; wrong.has_vtable = true;
        expect(!r::matches_family_layout(wrong), "vtable-bearing family layout is not this fork");
        wrong = layout; wrong.views = 8;
        expect(!r::matches_family_layout(wrong), "changed views offset rejected");
        wrong = layout; wrong.render_target = 0x20;
        expect(!r::matches_family_layout(wrong), "changed render target offset rejected");
        wrong = layout; wrong.scene_interface = 0x28;
        expect(!r::matches_family_layout(wrong), "changed scene offset rejected");
        expect(!r::resolve({}, base, base + r::callback_return_rva, entry, layout), "unreadable process evidence fails closed");
        expect(resolve(original) == base + r::entry_rva && layout == ktjl_renderer_test_layout(),
            "prior rejection does not poison retry or mutate the discovered SDK layout");
    }
}
