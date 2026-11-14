#pragma once

#include "mods/vr/GalacticRacerRHICommand.hpp"

void test_swgr_rhi_command_root() {
    namespace s = uevr::swgr_rhi;
    swgr_tests::Memory m;
    const auto b = m.base, constructor = b + 0x100;
    m.put(constructor, s::constructor_entry);
    m.put(constructor + 0x47, s::constructor_root_zero);
    m.put(constructor + 0x168, s::constructor_root_address);
    m.put(constructor + 0x21a, s::constructor_link_store);
    const auto layout = [&] { return s::command_list_layout(m.view(), constructor, s::constructor_size, b, m.bytes.size()); };
    expect(layout(), "SWGR source/binary constructor proves Root and tail-link offsets without a fixed RVA");
    const auto original = m;
    const auto mutate = [&](size_t offset, const auto& code) {
        for (size_t i = 0; i < code.size(); ++i) {
            m = original; m.bytes[constructor - b + offset + i] ^= 1;
            expect(!layout(), "every validated constructor byte is required before reading the new layout");
        }
    };
    mutate(0, s::constructor_entry); mutate(0x47, s::constructor_root_zero);
    mutate(0x168, s::constructor_root_address); mutate(0x21a, s::constructor_link_store);
    m = original;
    for (size_t size : {size_t{0}, s::constructor_size - 1, s::constructor_size + 1}) {
        expect(!s::command_list_layout(m.view(), constructor, size, b, m.bytes.size()), "changed/truncated unwind contract rejected");
    }
    expect(!s::command_list_layout(m.view(), constructor, s::constructor_size, b, 0x180), "constructor must fit its image");
    m.executable = false; expect(!layout(), "non-executable constructor cannot prove an ABI");
    m = original; m.readable = false; expect(!layout(), "unreadable constructor fails closed");

    m = original;
    const auto list = b + 0x3000, root = b + 0x4000, last = b + 0x4100, cursor = b + 0x4200;
    m.put(list, cursor); // Even a genuine-looking command at +0 is never read/hijacked.
    m.put(list + s::root_offset, s::Head{root, last + s::command_next_offset});
    m.put(root + s::command_next_offset, last);
    m.put(last + s::command_next_offset, uintptr_t{});
    int validations{};
    std::vector<uintptr_t> validated_commands;
    const auto valid = [&](uintptr_t p) {
        ++validations; validated_commands.push_back(p);
        return p == root || p == last || p == cursor;
    };
    const auto get = [&](bool enabled = true) { return s::command_root(m.view(), list, enabled, valid); };
    expect(get() == root && validations == 2 && validated_commands == std::vector<uintptr_t>{root, last},
        "even a valid-looking allocator cursor is ignored: only the proven Root/last commands are validated");
    const auto queued = m;
    validations = 0;
    expect(!get(false) && validations == 0, "unproven layout performs no command validation");
    m.put(list + s::root_offset, s::Head{0, list + s::root_offset});
    expect(get() == uintptr_t{} && validations == 0, "an empty queue is pending, not an invitation to scan the allocator");
    m.put(list + s::root_offset, s::Head{cursor + 0x100, cursor + 0x100 + s::command_next_offset});
    expect(!get(), "a readable but unowned command cannot establish a cached root");
    m = queued;
    expect(get() == root, "a later valid frame recovers without reinjection after an empty/rejected frame");
    m.put(list + s::root_offset, s::Head{root, root + s::command_next_offset});
    m.put(root + s::command_next_offset, uintptr_t{});
    expect(get() == root, "single-command queue uses its own null Next as the tail link");
    m.put(root + s::command_next_offset, root);
    expect(!get(), "self-referencing command is not a valid queue");
    m = queued; m.put(last + s::command_next_offset, root);
    expect(!get(), "tail link must point to a null Next");
    for (const auto bad : {uintptr_t{}, uintptr_t{1}, list + s::root_offset, root + 1, UINTPTR_MAX}) {
        m = queued; m.put(list + s::root_offset, s::Head{bad, last + s::command_next_offset});
        expect(!get(), "invalid/unaligned/noncanonical/header Root cannot be hooked");
        m = queued; m.put(list + s::root_offset, s::Head{root, bad});
        expect(!get(), "invalid tail link cannot authorize a Root");
    }
    m = queued; m.readable = false; expect(!get(), "unreadable queue fails closed without caching a layout result");
    for (const auto bad : {uintptr_t{}, uintptr_t{1}, list + 1, UINTPTR_MAX - 7}) {
        expect(!s::command_root(m.view(), bad, true, valid), "invalid command-list pointer cannot be read");
    }
    m = queued;
    expect(!s::command_root(m.view(), list, true, [&](uintptr_t p) {
        m.put(list + s::root_offset, s::Head{cursor, cursor + s::command_next_offset});
        return p == root || p == last;
    }), "changed queue identity is rejected before publication");
    m = queued;
    for (int i = 0; i < 10; ++i) { expect(get() == root, "healthy frames do not inherit the legacy analysis latch"); }
}
