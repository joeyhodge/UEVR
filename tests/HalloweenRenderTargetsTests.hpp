#pragma once

#include <fstream>
#include <iterator>
#include "mods/vr/HalloweenRenderTargets.hpp"
#include "mods/vr/HalloweenNativeFix.hpp"

namespace halloween_tests {
struct Memory {
    uintptr_t base{0x100000};
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x3000);
    uintptr_t external_code{};
    std::array<uint8_t, 32> external_bytes{};
    bool executable{true};
    bool readable{true};
    template<class T> void put(uintptr_t address, const T& value) {
        std::memcpy(bytes.data() + address - base, &value, sizeof(value));
    }
    sdk::discovery::Memory view() {
        return {this, [](void* context, uintptr_t address, void* out, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            if (m.readable && m.external_code && address >= m.external_code &&
                address - m.external_code <= m.external_bytes.size() &&
                size <= m.external_bytes.size() - (address - m.external_code)) {
                std::memcpy(out, m.external_bytes.data() + address - m.external_code, size);
                return true;
            }
            if (!m.readable || address < m.base || address - m.base > m.bytes.size() ||
                size > m.bytes.size() - (address - m.base)) { return false; }
            std::memcpy(out, m.bytes.data() + address - m.base, size);
            return true;
        }, [](void* context, uintptr_t address, size_t size) {
            auto& m = *static_cast<Memory*>(context);
            if (m.executable && m.external_code && address >= m.external_code &&
                address - m.external_code <= m.external_bytes.size() &&
                size <= m.external_bytes.size() - (address - m.external_code)) { return true; }
            return m.executable && address >= m.base && address - m.base <= 0x1000 && size <= 0x1000 - (address - m.base);
        }};
    }
};
}

void test_halloween_render_targets() {
    namespace h = uevr::halloween_rt;
    using uevr::games::is_halloween_ue574_dx12_runtime;
    expect(is_halloween_ue574_dx12_runtime(L"D:\\Games\\HALLOWEEN.exe", 0x50007, 0x40000, true), "Halloween exact 5.7.4 DX12 gate");
    expect(!is_halloween_ue574_dx12_runtime(L"Other.exe", 0x50007, 0x40000, true), "other UE5.7.4 games unchanged");
    expect(!is_halloween_ue574_dx12_runtime(L"Halloween.exe.bak", 0x50007, 0x40000, true), "Halloween basename lookalike rejected");
    expect(!is_halloween_ue574_dx12_runtime(L"Halloween.exe", 0x50007, 0x40000, false), "Halloween DX11 unchanged");
    for (const auto version : {0x50005u, 0x50006u, 0x50008u}) {
        expect(!is_halloween_ue574_dx12_runtime(L"Halloween.exe", version, 0x40000, true), "other engine minors unchanged");
    }
    expect(!is_halloween_ue574_dx12_runtime(L"Halloween.exe", 0x50007, 0x50000, true), "unproven patch version unchanged");

    halloween_tests::Memory m;
    const auto ret = m.base + 0x100;
    const auto join = ret + 0x228;
    m.put(ret - h::allocate_arguments.size(), h::allocate_arguments);
    m.put(ret, std::array<uint8_t, 8>{0x84,0xc0,0x0f,0x85,0x20,0x02,0,0});
    m.put(join, h::publish_rt);
    m.put(join + 0x2c, h::publish_srv);
    expect(h::allocation_join(m.view(), ret) == join, "observe completed refs instead of a pre-assignment finalize temporary");
    const auto release = m.base + 0x80;
    m.put(release, h::release_entry);
    for (auto offset : {0x27u, 0x56u}) {
        m.put(join + offset, uint8_t{0xe8});
        m.put(join + offset + 1, static_cast<int32_t>(release - (join + offset + 5)));
    }
    expect(h::allocation_release(m.view(), join) == release, "signed calls agree on the engine packed-refcount release");
    for (auto address : {join + 0x27, join + 0x57, release + 0x16}) {
        auto changed = m; changed.bytes[address - m.base] ^= 1;
        expect(!h::allocation_release(changed.view(), join), "changed release ABI/target must fail closed");
    }
    for (const auto address : {ret - 0x10, ret, ret + 2, join, join + 0x2c}) {
        auto changed = m; changed.bytes[address - m.base] ^= 1;
        expect(!h::allocation_join(changed.view(), ret), "changed allocator/output contract fails closed");
    }
    for (int32_t delta : {-8, 0, 0x800}) {
        auto changed = m; changed.put(ret + 4, delta);
        expect(!h::allocation_join(changed.view(), ret), "backward/unbounded post-allocation branches rejected");
    }
    m.executable = false;
    expect(!h::allocation_join(m.view(), ret), "data cannot be a code hook site");
    m.executable = true;

    const auto texture = m.base + 0x1800, wrapper = m.base + 0x1900, table = m.base + 0x1c00;
    const auto resource = m.base + 0x2800, getter = m.base + 0x800;
    m.put(texture, table); m.put(texture + 0xd0, wrapper); m.put(wrapper + 0x20, resource);
    m.put(table + 8 * sizeof(uintptr_t), getter); m.put(getter, h::native_getter);
    expect(h::native_resource(m.view(), texture) == resource, "read-only native chain needs a proven getter but never calls it");
    auto changed = m; changed.put(texture + 0xd0, uintptr_t{});
    expect(!h::native_resource(changed.view(), texture), "uninitialized native chain retries without invoking fallback virtual slots");
    changed = m; changed.put(table + 9 * sizeof(uintptr_t), getter);
    expect(!h::native_resource(changed.view(), texture), "ambiguous getter contracts rejected");
    changed = m; changed.bytes[getter - m.base + 9] ^= 8;
    expect(!h::native_resource(changed.view(), texture), "changed native layout rejected");
    h::NativeDescription desc{4944,2416,3,24,1,1,0,1,1};
    expect(h::valid_scene(desc, 4944, 2416), "single-sample packed scene accepted");
    expect(!h::valid_scene(desc, 1920, 1080), "desktop backbuffer is not the scene target");
    for (auto invalid : {h::NativeDescription{4944,2416,3,0,1,1,0,1,1},
                         h::NativeDescription{4944,2416,3,24,3,1,0,1,1},
                         h::NativeDescription{4944,2416,3,24,1,2,0,1,1},
                         h::NativeDescription{4944,2416,3,24,1,1,0,2,1}}) {
        expect(!h::valid_scene(invalid, 4944, 2416), "bad format/flags/MSAA/array scene rejected");
    }

    const auto renderer=m.base+0x1000, output=m.base+0x1100, graph=m.base+0x1200, input=m.base+0x1400;
    h::SlateInputs inputs{renderer,m.base+0x2000,m.base+0x2100,m.base+0x2200,0,0,0,0,2560,1440,1.0f,0};
    m.put(input, inputs); m.put(graph + 0xc0, m.base + 0x2400);
    const auto object = [&](uintptr_t p) { return p == renderer || p == inputs.window || p == inputs.viewport_info; };
    auto match = h::slate_inputs(m.view(), renderer, output, graph, input, object);
    expect(match && match->width == 2560 && match->height == 1440 && match->command_list == m.base+0x2400,
        "UE5.7 input omits PostProcessRequests; monitor extent and real command list are preserved");
    for (auto width : {1920, 3840}) {
        inputs.max_x=width; m.put(input, inputs);
        match=h::slate_inputs(m.view(), renderer, output, graph, input, object);
        expect(match && match->width == width, "UI extent is not hardcoded to 1080p");
    }
    expect(!h::slate_inputs(m.view(), renderer, graph, graph, input, object), "sret storage cannot alias RDGBuilder");
    m.put(graph+0xc0, output);
    expect(!h::slate_inputs(m.view(), renderer, output, graph, input, object), "sret output cannot become an RHI command list");
    m.put(graph+0xc0, m.base+0x2400);
    inputs.scale=std::numeric_limits<float>::quiet_NaN(); m.put(input, inputs);
    expect(!h::slate_inputs(m.view(), renderer, output, graph, input, object), "incomplete/corrupt Slate input rejected");
    m.readable=false;
    expect(!h::native_resource(m.view(), texture) && !h::allocation_join(m.view(), ret), "unreadable discovery never publishes partial results");
}

void test_halloween_native_family() {
    namespace h = uevr::halloween_native;
    halloween_tests::Memory m;
    const auto entry = m.base + 0x100, helper = m.base + 0x800, table = m.base + 0x2000;
    const auto call = [&](uintptr_t at, uintptr_t target) {
        m.put(at, uint8_t{0xe8});
        m.put(at + 1, static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 5)));
    };
    m.put(entry, h::copy_entry);
    m.put(entry + 0x18, std::array<uint8_t, 3>{0x48,0x8d,0x05});
    m.put(entry + 0x1b, static_cast<int32_t>(table - (entry + 0x1f)));
    m.put(entry + 0x1f, h::copy_arrays);
    m.put(entry + 0x35, h::all_views);
    m.put(entry + 0x42, h::targets);
    m.put(entry + 0xaf, h::additional);
    m.put(entry + 0x293, h::owned);
    m.put(entry + 0x318, h::copy_tail);
    m.put(helper, h::array_entry);
    m.put(helper + 0x5a, h::array_copy);
    m.put(helper + 0x6c, h::array_tail);
    call(entry + 0x30, helper); call(entry + 0x3d, helper);
    call(helper + 0x55, m.base + 0x900); call(helper + 0x67, m.base + 0x900);
    for (const auto offset : {0x68u,0x1deu,0x1ecu,0x258u,0x279u,0x305u,0x313u}) {
        call(entry + offset, m.base + 0x900);
    }
    const auto resolve = [&](halloween_tests::Memory& memory, size_t size = h::family_copy_size) {
        return h::family_copy_vtable(memory.view(), entry, size, m.base, m.bytes.size());
    };
    expect(resolve(m) == table, "Halloween outlined Views/AllViews copy validates without execution");
    expect(!resolve(m, 0x1fe), "unrelated StateTree-sized copy must not be accepted");
    expect(!resolve(m, h::family_copy_size - 1), "truncated unwind extent rejected");
    for (const auto address : {entry,entry+0x18,entry+0x20,entry+0x37,entry+0x42,entry+0xaf,
        entry+0x296,entry+0x320,helper+0x1b,helper+0x20,helper+0x60,helper+0x7e}) {
        auto changed = m; changed.bytes[address - m.base] ^= 1;
        expect(!resolve(changed), "changed copy ABI/layout/pointer stride must fail closed");
    }
    auto changed = m; changed.bytes[entry + 0x3e - m.base] ^= 1;
    expect(!resolve(changed), "Views and AllViews must use the same proven pointer-array copy");
    changed = m; changed.put(entry + 0x1b, int32_t{0x100000});
    expect(!resolve(changed), "vtable must stay inside the owning image");
    changed = m; changed.put(entry + 0x69, int32_t{0x100000});
    expect(!resolve(changed), "out-of-image member-copy call rejected");
    changed = m; changed.executable = false;
    expect(!resolve(changed), "non-executable constructor rejected");
    changed = m; changed.readable = false;
    expect(!resolve(changed), "unreadable constructor rejected");
}

void test_halloween_native_family_fixture(const char* path) {
    namespace h = uevr::halloween_native;
    struct Segment { uintptr_t address{}; std::vector<uint8_t> bytes{}; bool executable{}; };
    struct Image { std::vector<Segment> segments{}; } image;
    std::ifstream input(path, std::ios::binary);
    std::array<uint64_t, 5> header{}; // constructor, helper, vtable, image base, image size
    uint32_t count{};
    input.read(reinterpret_cast<char*>(header.data()), sizeof(header));
    input.read(reinterpret_cast<char*>(&count), sizeof(count));
    expect(input && count > 0 && count <= 32, "family fixture header is complete and bounded");
    if (!input || count == 0 || count > 32) { return; }
    for (uint32_t i = 0; i < count; ++i) {
        Segment segment;
        uint32_t size{}; uint8_t executable{};
        input.read(reinterpret_cast<char*>(&segment.address), sizeof(segment.address));
        input.read(reinterpret_cast<char*>(&size), sizeof(size));
        input.read(reinterpret_cast<char*>(&executable), sizeof(executable));
        if (!input || size == 0 || size > 0x1000) { expect(false, "bad family fixture segment"); return; }
        segment.bytes.resize(size); segment.executable = executable != 0;
        input.read(reinterpret_cast<char*>(segment.bytes.data()), size);
        if (!input) { expect(false, "truncated family fixture segment"); return; }
        image.segments.push_back(std::move(segment));
    }
    const sdk::discovery::Memory memory{&image,
        [](void* context, uintptr_t address, void* out, size_t size) {
            for (const auto& s : static_cast<Image*>(context)->segments) {
                if (h::in_module(address, size, s.address, s.bytes.size())) {
                    std::memcpy(out, s.bytes.data() + address - s.address, size); return true;
                }
            }
            return false;
        }, [](void* context, uintptr_t address, size_t size) {
            for (const auto& s : static_cast<Image*>(context)->segments) {
                if (s.executable && h::in_module(address, size, s.address, s.bytes.size())) { return true; }
            }
            return false;
        }};
    expect(h::family_copy_vtable(memory, header[0], h::family_copy_size, header[3], header[4]) == header[2],
        "real Halloween family constructor and helper pass production validation");
    image.segments.front().bytes[0x293] ^= 1;
    expect(!h::family_copy_vtable(memory, header[0], h::family_copy_size, header[3], header[4]),
        "changed real owned-interface copy fails closed");
}

// Captured caller bytes from either examined EXE can exercise the production
// resolver without loading the game, resolving imports, or executing any code.
void test_halloween_allocation_fixture(const char* path) {
    std::ifstream input(path, std::ios::binary);
    halloween_tests::Memory m;
    // Two original VAs, 0x600 caller bytes, and the 32-byte Release prologue.
    // Separate memory segments preserve the real signed CALL displacements.
    std::vector<uint8_t> fixture{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    expect(fixture.size() == 16 + 0x600 + 32, "Halloween fixture is complete");
    if (fixture.size() != 16 + 0x600 + 32) { return; }
    std::memcpy(&m.base, fixture.data(), 8);
    std::memcpy(&m.external_code, fixture.data() + 8, 8);
    m.bytes.assign(fixture.begin() + 16, fixture.begin() + 16 + 0x600);
    std::memcpy(m.external_bytes.data(), fixture.data() + 16 + 0x600, 32);
    const auto join = uevr::halloween_rt::allocation_join(m.view(), m.base+0x100);
    expect(join.has_value(),
        "real EXE allocation fixture passes the production completed-output contract");
    expect(join && uevr::halloween_rt::allocation_release(m.view(), *join) == m.external_code,
        "real EXE releases both output refs through the validated same helper");
}
