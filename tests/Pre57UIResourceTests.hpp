#pragma once

#include "mods/vr/Pre57SlateSafety.hpp"
#include "utility/ReadOnlyPointerAccessor.hpp"
#include "utility/DiagnosticChange.hpp"

namespace pre57_ui_tests {
struct Getter {
    static constexpr uintptr_t function = 0x180001000, object = 0x200000, wrapper = 0x300000, native = 0x400000;
    std::vector<uint8_t> code{0x48, 0x8b, 0x81, 0xb8, 0, 0, 0, 0x48, 0x8b, 0x40, 0x20, 0xc3};
    uintptr_t resource{wrapper}, result{native};
    size_t reads{};
    sdk::discovery::Memory memory() {
        return {this,
            [](void* context, uintptr_t address, void* out, size_t size) {
                auto& self = *static_cast<Getter*>(context);
                ++self.reads;
                if (address >= function && address - function <= self.code.size() && size <= self.code.size() - (address - function)) {
                    std::memcpy(out, self.code.data() + address - function, size);
                    return true;
                }
                if (size != 8) {
                    return false;
                }
                if (address == object + 0xb8) {
                    std::memcpy(out, &self.resource, 8);
                    return true;
                }
                if (address == wrapper + 0x20) {
                    std::memcpy(out, &self.result, 8);
                    return true;
                }
                return false;
            },
            [](void* context, uintptr_t address, size_t size) {
                const auto& self = *static_cast<Getter*>(context);
                return address >= function && address - function <= self.code.size() && size <= self.code.size() - (address - function);
            }};
    }
    std::optional<uintptr_t> run() { return utility::accessor::read_pointer(memory(), function, object); }
};
} // namespace pre57_ui_tests

void test_pre57_ui_resources() {
    using G = pre57_ui_tests::Getter;
    G getter;
    expect(getter.run() == G::native, "native getter is interpreted without calling a game function");
    // Stock two-stage D3D12 getter, with its virtual backbuffer fallback left unexecuted.
    const std::vector<uint8_t> stock{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xb8, 0, 0, 0, 0x33, 0xdb, 0x48, 0x85, 0xc0,
        0x74, 0x09, 0x48, 0x8b, 0x58, 0x20, 0x48, 0x85, 0xdb, 0x75, 0x21, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x38, 0x48, 0x85, 0xc0, 0x74, 0x16,
        0x48, 0x8b, 0x80, 0xb8, 0, 0, 0, 0x48, 0x85, 0xc0, 0x74, 0x0a, 0x48, 0x8b, 0x40, 0x20, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3, 0x48,
        0x8b, 0xc3, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
    getter.code = stock;
    expect(getter.run() == G::native, "stock getter null tests and balanced private stack preserve the initialized resource");
    getter.resource = 0;
    expect(!getter.run(), "pending resource does not execute its linked-texture virtual fallback");
    getter = G{};
    getter.code = stock;
    getter.result = 0;
    expect(!getter.run(), "null native does not execute a fallback even when the wrapper exists");
    getter = G{};
    getter.code.insert(getter.code.begin(), {0x48, 0x89, 0x51, 0x08});
    expect(
        !getter.run() && getter.resource == G::wrapper, "destructor or hidden-output writes are rejected without touching object memory");
    for (const auto& code : {std::vector<uint8_t>{0xc3}, {0x48, 0x89, 0xc8, 0xc3}, {0x48, 0x8d, 0x41, 0x20, 0xc3}, {0xe8, 0, 0, 0, 0, 0xc3},
             {0xff, 0x50, 0x20, 0xc3}, {0xeb, 0xfe}, {0x48, 0x8b, 0x02, 0xc3}, {0x48, 0x89, 0x01, 0xc3}, {0x48, 0x8b, 0x81, 0xb8, 0, 0},
             {0x65, 0x48, 0x8b, 0x01, 0xc3}}) {
        getter = G{};
        getter.code = code;
        expect(!getter.run() && getter.reads < 1024, "unsupported/looping/truncated accessors are bounded rejections, not candidate calls");
    }
    getter = G{};
    getter.result = UINTPTR_MAX;
    expect(!getter.run(), "noncanonical native pointers are rejected");
    getter = G{};
    getter.code.back() = 0xc2;
    getter.code.insert(getter.code.end(), {0, 0});
    expect(getter.run() == G::native, "RET 0 keeps the x64 getter stack contract");
    getter.code[getter.code.size() - 2] = 8;
    expect(!getter.run(), "nonzero stack cleanup is not accepted as an x64 pointer getter");
}

void test_pre57_ui_safety() {
    utility::logging::DiagnosticChanges<2, 2> logs;
    expect(logs.should_log(true, 1, {10, 20}) && !logs.should_log(true, 1, {10, 20}),
        "unchanged Glory UI status is quiet after its first observation");
    expect(logs.should_log(true, 2, {10, 20}) && !logs.should_log(true, 1, {10, 20}) && !logs.should_log(true, 2, {10, 20}),
        "alternating diagnostic sources do not make stable UI messages spam");
    expect(logs.should_log(true, 1, {30, 40}) && logs.should_log(true, 1, {10, 20}),
        "target/extent changes and returns are observable, not suppressed forever");
    expect(!logs.should_log(true, 3, {10, 20}) && !logs.should_log(true, 1, {10, 20}),
        "diagnostic channels are bounded without evicting steady-state entries");
    expect(logs.should_log(false, 1, {10, 20}) && logs.should_log(false, 1, {10, 20}),
        "other games retain their original time-limited diagnostic behavior");
    utility::logging::DiagnosticChanges<2, 2> other_thread;
    expect(other_thread.should_log(true, 1, {10, 20}), "thread-confined log state has no cross-thread shared timestamps or cached resources");
    namespace s = uevr::pre57_slate;
    using F = pre57_slate_tests::Fixture;
    F registration;
    s::FunctionRange draw{F::begin, F::begin + registration.code.size(), F::base};
    s::FunctionRange callee{F::target, F::target + registration.callee.size(), F::base};
    const auto discover = [&] {
        return s::discover_raw_output(
            F::begin, {F::base, 0x10000}, registration.original, {s::glory_raw_registration, s::glory_raw_registration_mask},
            [&](uintptr_t p, void* out, size_t count) {
                if (p == F::begin && count == registration.code.size()) {
                    std::memcpy(out, registration.code.data(), count);
                    return true;
                }
                if (p == F::name && count == sizeof(registration.text)) {
                    std::memcpy(out, registration.text.data(), count);
                    return true;
                }
                if (p == F::target && count == registration.callee.size()) {
                    std::memcpy(out, registration.callee.data(), count);
                    return true;
                }
                return false;
            },
            [&](uintptr_t p, size_t count) {
                return (p == F::begin && count == registration.code.size()) || (p == F::target && count == registration.callee.size());
            },
            [&](uintptr_t p) -> std::optional<s::FunctionRange> {
                if (p == F::begin) {
                    return draw;
                }
                if (p == F::target) {
                    return callee;
                }
                return {};
            });
    };
    const auto normal_draw = draw, normal_callee = callee;
    expect(discover().match.has_value(),
        "generic observation requires complete unwind-owned caller and callee contracts without per-EXE addresses");
    for (int invalid = 0; invalid < 8; ++invalid) {
        draw = normal_draw;
        callee = normal_callee;
        switch (invalid) {
        case 0:
            --draw.begin;
            break;
        case 1:
            draw.end = draw.begin;
            break;
        case 2:
            draw.image_base += 0x1000;
            break;
        case 3:
            draw.end = draw.begin + 0x10001;
            break;
        case 4:
            --callee.begin;
            break;
        case 5:
            --callee.end;
            break;
        case 6:
            ++callee.image_base;
            break;
        case 7:
            callee.end = callee.begin;
            break;
        }
        expect(
            !discover().match, "missing, neighboring, oversized, truncated or foreign function owners cannot install a generic observer");
    }
    draw = normal_draw;
    callee = normal_callee;
    using E = s::EngineContract;
    for (const auto version : {L"5.5.0", L"5.5.1", L"5.5.4-123", L"5.6.1+Licensee"}) {
        expect(s::engine_contract(version, 0x50008) == E::RDG55_56, "embedded minor version wins over conflicting file metadata");
    }
    expect(s::engine_contract(L"5.4.4", 0) == E::Immediate54 && s::engine_contract(L"", 0x50004) == E::Immediate54,
        "UE5.4 is an independent immediate-command-list adapter, not an RDG allowlist expansion");
    expect(s::engine_contract(L"", 0x50005) == E::RDG55_56 && s::engine_contract(L"0.00", 0x50006) == E::RDG55_56,
        "file minor fallback is limited to missing embedded metadata");
    for (const auto version : {L"4.27.2", L"5.3.2", L"5.7.4", L"5.8.4", L"6.0.0", L"5.50.0", L"5.60", L"5.6x", L"5..6", L"unknown"}) {
        expect(s::engine_contract(version, 0x50005) == E::Unsupported,
            "unsupported or malformed embedded versions cannot borrow a neighboring adapter");
    }
    s::TextureFacts facts{0x10000, 0x20000, 0x30000, 0x30000, 0x40000, 0x50000};
    facts.desc = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1920, 1080, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    const auto valid = facts;
    expect(s::classify(facts) == s::ObservationState::EngineOwnedCandidate,
        "a distinct valid texture is only a candidate, not permission to publish or copy");
    facts.native = facts.scene_native;
    expect(s::classify(facts) == s::ObservationState::SyntheticRequired,
        "distinct RHI wrappers aliasing one scene resource are not engine-owned UI");
    facts = valid;
    facts.texture = facts.scene_texture;
    expect(s::classify(facts) == s::ObservationState::SyntheticRequired, "exact scene RHI aliases require a separate synthetic adapter");
    facts = valid;
    facts.scene_native = 0;
    expect(s::classify(facts) == s::ObservationState::Observing, "unknown scene relation is not silently accepted as distinct");
    for (int mutation = 0; mutation < 17; ++mutation) {
        facts = valid;
        switch (mutation) {
        case 0:
            facts.native = 0;
            break;
        case 1:
            facts.expected_device = 0;
            break;
        case 2:
            facts.device += 8;
            break;
        case 3:
            facts.desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            break;
        case 4:
            facts.desc.Width = 0;
            break;
        case 5:
            facts.desc.Height = 16385;
            break;
        case 6:
            facts.desc.DepthOrArraySize = 2;
            break;
        case 7:
            facts.desc.MipLevels = 2;
            break;
        case 8:
            facts.desc.SampleDesc.Count = 4;
            break;
        case 9:
            facts.desc.SampleDesc.Quality = 1;
            break;
        case 10:
            facts.desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        case 11:
            facts.desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
        case 12:
            facts.desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            break;
        case 13:
            facts.desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            break;
        case 14:
            facts.desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            break;
        case 15:
            facts.desc.Width = UINT64_MAX;
            break;
        case 16:
            facts.desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
            break;
        }
        expect(s::classify(facts) == s::ObservationState::Rejected,
            "wrong device, HDR, arrays, MSAA and unsupported descriptor layouts fail closed");
    }
    s::Epoch epoch{1, 2, 3, 4, 5, 6, 7, {1920, 1080}, 0};
    s::Observation observation;
    observation.observe(epoch, valid, 1000);
    observation.observe(epoch, valid, 2000);
    expect(observation.stable == 2, "same-epoch observation stability counts bounded samples, not render calls");
    for (int field = 0; field < 10; ++field) {
        auto changed = epoch;
        switch (field) {
        case 0:
            ++changed.module;
            break;
        case 1:
            ++changed.draw;
            break;
        case 2:
            ++changed.renderer;
            break;
        case 3:
            ++changed.window;
            break;
        case 4:
            ++changed.viewport;
            break;
        case 5:
            ++changed.scene;
            break;
        case 6:
            ++changed.device;
            break;
        case 7:
            ++changed.extent.width;
            break;
        case 8:
            ++changed.extent.height;
            break;
        case 9:
            ++changed.rendering_method;
            break;
        }
        observation.observe(changed, valid, 3000);
        expect(observation.stable == 1, "module, window, scene, device, mode and resize epochs cannot reuse prior observations");
        observation.observe(epoch, valid, 4000);
    }
    observation.observe(epoch, valid, 8000);
    expect(observation.stable == 1, "stale samples do not establish lifetime or GPU retirement");
    observation.reset();
    expect(observation.state == s::ObservationState::Off && !observation.facts.native && !observation.stable,
        "disable/reset drops value-only observations without retaining or releasing engine/GPU objects");
    observation.observe(epoch, valid, 9000);
    facts = valid;
    facts.device = 0;
    observation.observe(epoch, facts, 10000);
    expect(observation.stable == 0 && observation.state == s::ObservationState::Rejected,
        "a later rejected resource immediately invalidates a prior valid observation");

    constexpr uintptr_t base = 0x80000, renderer = 0xa0000, main_window = 0xb0000, main_viewport = 0xc0000;
    std::array<s::InputPrefix, 3> inputs{};
    for (size_t i = 0; i < inputs.size(); ++i) {
        inputs[i].renderer = renderer;
        inputs[i].element_list = 0xd0000;
        inputs[i].window = main_window + i * 0x100;
        inputs[i].viewport = main_viewport + i * 0x100;
        inputs[i].min_x = -10;
        inputs[i].min_y = -20;
        inputs[i].max_x = 1910;
        inputs[i].max_y = 1060;
        inputs[i].ui_scale = -1; // Valid game sentinel, not an invalid scale.
    }
    std::swap(inputs[0], inputs[2]);
    size_t reads{};
    const auto read = [&](uintptr_t p, void* output, size_t count) {
        ++reads;
        if (p < base || p - base > sizeof(inputs) || count > sizeof(inputs) - (p - base)) {
            return false;
        }
        std::memcpy(output, reinterpret_cast<const uint8_t*>(inputs.data()) + p - base, count);
        return true;
    };
    const s::MainWindow main{main_window, main_viewport, true};
    const auto match = s::select_main_window(base, 3, sizeof(s::InputPrefix), main, renderer, read);
    expect(match && match->address == base + 2 * sizeof(s::InputPrefix) && match->extent == s::Extent{1920, 1080} && reads == 3,
        "bounded batch inspection uniquely selects a pinned main window, not the first element");
    expect(!s::select_main_window(base, 3, 0, main, renderer, read) &&
               !s::select_main_window(base, 3, sizeof(s::InputPrefix), {main_window, main_viewport, false}, renderer, read),
        "missing compiled stride or unretained window cannot authorize a batched input ABI");
    inputs[0] = inputs[2];
    expect(!s::select_main_window(base, 3, sizeof(s::InputPrefix), main, renderer, read), "ambiguous main-window inputs are rejected");
    expect(!s::select_main_window(base, 17, sizeof(s::InputPrefix), main, renderer, read) &&
               !s::select_main_window(UINTPTR_MAX, 3, sizeof(s::InputPrefix), main, renderer, read),
        "batch work and address arithmetic are bounded");
}

void test_pre57_ui_getter_fixture(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0 || file.tellg() > 512 * 1024) {
        expect(false, "bounded getter fixture");
        return;
    }
    file.seekg(0);
    const auto json = nlohmann::json::parse(file);
    size_t valid{};
    for (const auto& record : json.at("records")) {
        pre57_ui_tests::Getter getter;
        getter.code.clear();
        const auto value = record.at("code_hex").get<std::string>();
        if (value.size() % 2 || value.size() > 512) {
            expect(false, "bounded getter bytes");
            return;
        }
        for (size_t i = 0; i < value.size(); i += 2) {
            getter.code.push_back(static_cast<uint8_t>(std::stoul(value.substr(i, 2), nullptr, 16)));
        }
        const auto result = getter.run();
        if (result) {
            ++valid;
        }
        expect(record.at("slot").get<unsigned>() == 4 ? result == getter.native : !result,
            "actual Glory vtable code distinguishes its initialized native getter from bindless, output and constructor methods without "
            "calls");
    }
    expect(valid == 1, "one native accessor in the captured shipping texture vtable");
}
