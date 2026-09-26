#pragma once

#include <fstream>
#include <iterator>
#include "mods/vr/HalloweenRenderTargets.hpp"

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
