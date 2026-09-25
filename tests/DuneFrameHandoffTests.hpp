#include "mods/vr/DuneFrameHandoff.hpp"

void test_dune_frame_handoff() {
    namespace d = uevr::dune_frame;
    expect(uevr::games::is_dune_ue521_frame_handoff_runtime(
        L"D:\\Steam\\DuneSandbox-Win64-Shipping.exe", 0x50002, 0x10000), "Dune exact basename/version accepted");
    expect(!uevr::games::is_dune_ue521_frame_handoff_runtime(
        L"DuneSandbox-Win64-Shipping.exe.bak", 0x50002, 0x10000), "Dune substring lookalike rejected");
    for (uint32_t ms : {0x50001u, 0x50003u, 0x4001bu}) {
        expect(!uevr::games::is_dune_ue521_frame_handoff_runtime(
            L"DuneSandbox-Win64-Shipping.exe", ms, 0x10000), "Dune other engines unchanged");
    }
    expect(!uevr::games::is_dune_ue521_frame_handoff_runtime(
        L"FortSolis.exe", 0x50002, 0x10000), "other UE5.2.1 titles unchanged");
    expect(!uevr::games::is_dune_ue521_frame_handoff_runtime(
        L"DuneSandbox-Win64-Shipping.exe", 0x50002, 0x20000), "unvalidated Dune engine patch unchanged");
    for (unsigned mask = 0; mask < 32; ++mask) {
        expect(d::enabled(mask & 1, mask & 2, mask & 4, mask & 8, mask & 16) == (mask == 31),
            "Dune handoff requires validated DX12/OpenXR/Native/Native Fix");
    }
    expect(d::should_publish(4, 4, 100, 0, 0), "first executed command publishes its real frame");
    expect(d::should_publish(4, 4, 101, 4, 100), "subsequent execution advances frame identity");
    expect(!d::should_publish(4, 4, 100, 4, 100), "duplicate execution cannot republish the same frame");
    expect(!d::should_publish(4, 4, 99, 4, 100), "out-of-order completion cannot regress the clock");
    expect(!d::should_publish(5, 4, 101, 4, 100), "retired capture generation cannot publish after a transition");
    expect(!d::should_publish(0, 0, 101, 0, 100), "uninitialized capture cannot publish");
    expect(d::should_publish(5, 5, 10, 4, 100), "new generation may restart its frame sequence");
    expect(d::should_publish(5, 5, 10, 5, 0xFFFFFFFEu), "unsigned frame rollover advances safely");

    constexpr uintptr_t base = 0x140000000;
    int reads{};
    uint32_t corrupt_rva{};
    auto read_code = [&](uintptr_t address, void* out, size_t size) {
        ++reads;
        const auto copy = [&](uint32_t rva, const auto& code) {
            if (address != base + rva || size != code.size()) { return false; }
            std::memcpy(out, code.data(), size);
            if (rva == corrupt_rva) { static_cast<uint8_t*>(out)[size - 1] ^= 1; }
            return true;
        };
        return copy(d::renderer_calls_rva, d::renderer_calls) ||
            copy(d::graph_list_store_rva, d::graph_list_store) || copy(d::command_link_rva, d::command_link);
    };
    expect(d::validate_binary(base, d::image_timestamp, d::image_size, read_code), "all Dune binary contracts match");
    expect(reads == 3, "binary proof uses three bounded reads, not a process-wide scan");
    reads = 0;
    expect(!d::validate_binary(base, d::image_timestamp + 1, d::image_size, read_code) && reads == 0,
        "unknown update fails before reading fixed addresses");
    expect(!d::validate_binary(base, d::image_timestamp, d::image_size - 0x1000, read_code) && reads == 0,
        "old image does not inherit current addresses");
    for (auto rva : {d::renderer_calls_rva, d::graph_list_store_rva, d::command_link_rva}) {
        corrupt_rva = rva;
        expect(!d::validate_binary(base, d::image_timestamp, d::image_size, read_code), "each independent contract is mandatory");
    }

    constexpr uintptr_t heap = 0x100000, family = heap + 0x1000, views = heap + 0x2000;
    constexpr uintptr_t target = heap + 0x4000, scene = heap + 0x5000, view = heap + 0x8000;
    constexpr uintptr_t graph = heap + 0x18000, list = heap + 0x19000;
    constexpr uintptr_t root = heap + 0x1A000, tail = heap + 0x1B000, table = 0x200000;
    std::vector<uint8_t> memory(0x20000);
    auto write = [&](uintptr_t address, auto value) { std::memcpy(memory.data() + address - heap, &value, sizeof(value)); };
    auto read = [&](uintptr_t address, void* out, size_t size) {
        if (address < heap || address - heap > memory.size() || size > memory.size() - (address - heap)) { return false; }
        std::memcpy(out, memory.data() + address - heap, size);
        return true;
    };
    auto valid = [&](uintptr_t object) {
        uintptr_t value{};
        return read(object, &value, sizeof(value)) && value == table;
    };
    for (auto object : {family, target, scene, view, root, tail}) { write(object, table); }
    write(family + 8, views); write(family + 0x10, int32_t{1}); write(family + 0x14, int32_t{2});
    write(family + 0x20, target); write(family + 0x28, scene); write(family + 0x84, uint32_t{100});
    write(views, view); write(view + 0x10, family); write(view + 0x13A0, int32_t{1});
    const auto initial = memory;
    expect(d::read_family(family, target, read, valid).has_value(), "live-shaped primary family accepted");
    expect(!d::read_family(view, target, read, valid), "view argument cannot be treated as family");
    expect(!d::read_family(family, target + 8, read, valid), "auxiliary target rejected");
    expect(!d::read_family(family + 1, target, read, valid), "unaligned family rejected");
    expect(memory == initial, "family validation is read-only");
    for (int32_t bad_count : {0, -1, 3, 17}) {
        write(family + 0x10, bad_count);
        expect(!d::read_family(family, target, read, valid), "invalid/truncated view array rejected");
    }
    write(family + 0x10, int32_t{1}); write(view + 0x10, family + 8);
    expect(!d::read_family(family, target, read, valid), "stale owning-family backlink rejected");
    write(view + 0x10, family); write(view + 0x13A0, int32_t{256});
    expect(!d::read_family(family, target, read, valid), "unvalidated stereo pass rejected");
    write(view + 0x13A0, int32_t{0});
    expect(d::read_family(family, target, read, valid).has_value(), "full pass before capture remains acceptable");
    write(family + 0x84, uint32_t{101});
    expect(d::read_family(family, target, read, valid)->frame == 101, "failed observations do not poison later frames");
    write(graph + 0x50, list); write(list, root); write(list + 8, tail + 8);
    write(root + 8, tail); write(tail + 8, uintptr_t{});
    const auto command_initial = memory;
    expect(d::read_tail(graph, read, valid) == tail, "exact graph+0x50 list and terminal link resolve the command");
    expect(memory == command_initial, "command discovery never alters engine memory");
    write(graph + 0x50, uintptr_t{}); write(graph, list);
    expect(!d::read_tail(graph, read, valid), "stock graph+0 is not guessed for this fork");
    write(graph + 0x50, list); write(list + 8, list);
    expect(!d::read_tail(graph, read, valid), "empty Root link rejected");
    write(list + 8, tail + 8); write(tail + 8, root);
    expect(!d::read_tail(graph, read, valid), "nonterminal link rejected");
    write(tail + 8, uintptr_t{}); write(tail, uintptr_t{});
    expect(!d::read_tail(graph, read, valid), "bad command vtable rejected");
    write(tail, table); write(root + 8, uintptr_t{});
    expect(!d::read_tail(graph, read, valid), "disconnected root/tail rejected");
    write(list + 8, root + 8);
    expect(d::read_tail(graph, read, valid) == root, "single queued command supported");

    d::PendingFrames<2> pending{};
    expect(pending.reserve({root, table, 100, 1}), "first pending identity reserved");
    expect(!pending.reserve({root, table, 101, 2}), "duplicate cannot overwrite pending frame or generation");
    expect(pending.reserve({tail, table, 102, 1}), "second pending identity reserved");
    expect(!pending.reserve({tail + 0x80, table, 103, 1}), "full pool does not evict an in-flight command");
    const auto first = pending.take(root);
    expect(first && first->frame == 100 && first->generation == 1 && first->vtable == table,
        "execution consumes the exact original token");
    expect(!pending.take(root) && pending.contains(tail), "exactly-once retirement leaves other commands intact");
    expect(pending.reserve({root, table, 104, 2}), "retired storage supports later generations");
    expect(pending.take(tail).has_value() && pending.take(root).has_value(), "all commands retire independently of mode toggles");
}
