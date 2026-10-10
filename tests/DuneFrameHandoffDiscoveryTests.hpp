#include "mods/vr/DuneFrameHandoffDiscovery.hpp"

void test_dune_frame_handoff_discovery() {
    namespace d = uevr::dune_frame;
    namespace r = uevr::dune_renderer;
    std::array<uint8_t, r::maximum_abi_bytes> renderer{};
    const auto place = [](auto& bytes, size_t offset, const auto& value) {
        std::copy(value.begin(), value.end(), bytes.begin() + offset);
    };
    const std::array<uint8_t, 3> alias{0x4D,0x8B,0xF0}, data{0x4D,0x8B,0x26};
    const std::array<uint8_t, 12> count{0x49,0x63,0x46,0x08,0x49,0x83,0xC6,0x08,0x4D,0x8D,0x3C,0xC4};
    const std::array<uint8_t, 6> frame{0x89,0x83,0x84,0,0,0};
    place(renderer, 0x3B, alias); place(renderer, 0x97, data);
    place(renderer, 0x2AF, count); place(renderer, 0x2D3, frame);
    expect(r::valid_array_view_abi(renderer) && r::valid_r14_family_abi(renderer),
        "October R14 alias, array bounds, and family frame store prove the renderer ABI");
    for (auto offset : {0x3Bu, 0x97u, 0x2AFu, 0x2D3u}) {
        auto changed = renderer; changed[offset] ^= 1;
        expect(!r::valid_array_view_abi(changed), "every R14 ABI observation is required");
    }
    expect(!r::valid_array_view_abi(std::span{renderer}.first(0x100)) &&
        !r::valid_array_view_abi(std::span{renderer}.first(0x2D8)), "truncated renderer proofs fail closed");
    std::array<uint8_t, 7> retail{0x4D,0x8B,0x20,0x49,0x63,0x40,0x08};
    std::array<uint8_t, 10> test_build{0x49,0x8B,0xF8,0x4C,0x8B,0x27,0x48,0x63,0x47,0x08};
    expect(r::valid_array_view_abi(retail) && r::valid_array_view_abi(test_build), "both earlier compiler allocations remain supported");
    expect(!r::valid_array_view_abi(std::span{test_build}.subspan(3)), "RDI data/count reads without the R8 alias are rejected");
    retail[6] = 0x10;
    expect(!r::valid_array_view_abi(retail), "wrong array count field is not accepted");

    // Independent RVA shifts change CALL displacements as well as image location.
    for (const uintptr_t base : {uintptr_t{0x140000000}, uintptr_t{0x7FF800000000}}) {
        for (const size_t shift : {size_t{0}, size_t{0x1000}}) {
            constexpr size_t image_size = 0x10000;
            const d::CodeImage image{base, image_size};
            const auto bridge = base + 0x2000 + shift, graph = base + 0x4000 + shift / 2;
            const auto dispatcher = base + 0x6000 + shift / 4, allocator = base + 0x8000 + shift / 8;
            const auto renderer_entry = base + 0x1000;
            std::vector<uint8_t> memory(image_size);
            auto put = [&](uintptr_t address, const auto& bytes) { place(memory, address - base, bytes); };
            put(bridge, d::bridge_code); put(graph, d::graph_constructor_code);
            put(dispatcher, d::dispatcher_prefix);
            constexpr size_t callback_offset = 0xC7;
            put(dispatcher + callback_offset - 7, std::array<uint8_t, 7>{0x48,0x8B,0x83,0xD0,0,0,0});
            put(dispatcher + callback_offset, d::renderer_calls);
            auto call = [&](uintptr_t address, uintptr_t target) {
                const auto displacement = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(address + 5));
                std::memcpy(memory.data() + address - base + 1, &displacement, sizeof(displacement));
            };
            call(bridge + 57, graph); call(bridge + 72, dispatcher); call(graph + 36, allocator);
            const auto baseline = memory;
            size_t reads{};
            auto read = [&](uintptr_t address, void* output, size_t size) {
                ++reads;
                if (!image.contains(address, size)) { return false; }
                std::memcpy(output, memory.data() + address - base, size); return true;
            };
            auto entry = [&](uintptr_t address, size_t size) {
                return image.contains(address, size) &&
                    (address == renderer_entry || address == graph || address == dispatcher || address == allocator);
            };
            bool containing = true, owns = true;
            const auto contains = [&](uintptr_t address, size_t size) { return containing && address == bridge && size == d::bridge_code.size(); };
            const auto owned = [&](uintptr_t owner, uintptr_t address, size_t size) {
                return owns && owner == dispatcher && address == dispatcher + callback_offset - 7 && size == d::renderer_calls.size() + 7;
            };
            const std::array candidates{bridge};
            const auto discover = [&](std::span<const uintptr_t> locations) {
                return d::discover_contract(image, renderer_entry, renderer, locations, read, entry, contains, owned);
            };
            const auto proof = discover(candidates);
            expect(proof && proof->graph_constructor == graph && proof->dispatcher == dispatcher &&
                proof->renderer_calls == dispatcher + callback_offset, "relocation follows calls, not a build timestamp or old RVAs");
            expect(reads == 3 && memory == baseline, "discovery performs three bounded read-only observations");
            const std::array ambiguous{bridge, bridge};
            reads = 0;
            expect(!discover(ambiguous) && reads == 0, "ambiguous bridge is rejected before memory reads");
            expect(!discover(std::span<const uintptr_t>{}) && reads == 0, "missing bridge is rejected before memory reads");
            containing = false;
            expect(!discover(candidates), "command bridge must belong to one executable unwind range");
            containing = true; owns = false;
            expect(!discover(candidates), "callback bytes in an unrelated function cannot prove the ABI");
            owns = true;
            for (size_t i = 0; i < d::bridge_code.size(); ++i) {
                if (d::displacement_byte(i, d::bridge_displacements)) { continue; }
                memory = baseline; memory[bridge - base + i] ^= 1;
                expect(!discover(candidates), "every non-address bridge byte is mandatory");
            }
            for (size_t i = 0; i < d::graph_constructor_code.size(); ++i) {
                if (d::displacement_byte(i, d::graph_displacements)) { continue; }
                memory = baseline; memory[graph - base + i] ^= 1;
                expect(!discover(candidates), "changed RDG/RHI fields or argument registers fail closed");
            }
            for (size_t i = 0; i < d::renderer_calls.size(); ++i) {
                memory = baseline; memory[dispatcher - base + callback_offset + i] ^= 1;
                expect(!discover(candidates), "changed callback slots, family address or view stride fail closed");
            }
            memory = baseline;
            put(dispatcher + 0x140, d::renderer_calls);
            expect(!discover(candidates), "duplicate callback cluster fails closed");
            memory = baseline; call(bridge + 57, base + image_size);
            expect(!discover(candidates), "out-of-image constructor is rejected");
            memory = baseline; call(bridge + 72, graph);
            expect(!discover(candidates), "constructor cannot masquerade as the dispatcher");
            memory = baseline; call(graph + 36, graph);
            expect(!discover(candidates), "constructor recursion does not establish allocator ownership");
            auto failed_read = [](uintptr_t, void*, size_t) { return false; };
            expect(!d::discover_contract(image, renderer_entry, renderer, candidates, failed_read, entry, contains, owned),
                "unreadable code preserves the existing path");
            expect(!d::discover_contract(image, renderer_entry, test_build, candidates, read, entry, contains, owned),
                "legacy renderer ABI alone cannot enable relocatable RDG handoff");
            expect(!d::CodeImage{UINTPTR_MAX - 10, image_size}.contains(UINTPTR_MAX - 5, 4), "module end overflow is rejected");
            expect(!image.contains(base + image_size, 1) && !image.contains(base - 1, 1), "all proof reads stay inside the executable image");
        }
    }
}
