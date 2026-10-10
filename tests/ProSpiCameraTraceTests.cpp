#include "mods/vr/ProSpiCameraTrace.hpp"
#include "mods/vr/ProSpiNativeCameraLayout.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <unordered_map>

using namespace uevr::prospi::trace;
namespace {
void require(bool condition, const char* message) { if (!condition) { throw std::runtime_error(message); } }
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("uevr-prospi-trace-" + std::to_string(Recorder::clock_ns()));
    Fixture() { std::filesystem::create_directory(root); }
    ~Fixture() {
        if (root.parent_path() == std::filesystem::temp_directory_path() && root.filename().string().starts_with("uevr-prospi-trace-")) {
            std::error_code ec; std::filesystem::remove_all(root, ec);
        }
    }
    std::vector<nlohmann::json> events() {
        std::vector<nlohmann::json> out;
        for (const auto& e : std::filesystem::recursive_directory_iterator(root)) {
            if (e.path().filename() != "events.jsonl") { continue; }
            std::ifstream file(e.path()); std::string line;
            while (std::getline(file, line)) { out.push_back(nlohmann::json::parse(line)); }
        }
        return out;
    }
};
Camera sample() {
    Camera c{};
    c.input.valid = true; c.input.location = {100, 200, 250}; c.input.rotation = {-12, 179, 0}; c.input.fov = 20;
    c.world = 1; c.camera_manager = 2; c.focus_after = 1800; c.effective_fov = 20; c.base_fov = 90;
    copy_text(c.id, "camera");
    return c;
}
void settle(Recorder& r) {
    for (int i = 0; i < 300 && r.counters().status == Status::stopping; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
    require(r.counters().status != Status::stopping, "writer failed to finish asynchronously");
}
struct LayoutFixture {
    uintptr_t base{0x140000000};
    uint32_t timestamp{};
    size_t image_size{}, reads{};
    std::unordered_map<uintptr_t, uint8_t> memory;
    void bytes(uint32_t rva, std::string_view hex) {
        require(hex.size() % 2 == 0, "Odd code fixture");
        const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
        for (size_t i = 0; i < hex.size() / 2; ++i) {
            memory[base + rva + i] = (uint8_t)((digit(hex[i * 2]) << 4) | digit(hex[i * 2 + 1]));
        }
    }
    template <typename T> void put(uint32_t rva, T value) {
        const auto data = (const uint8_t*)&value;
        for (size_t i = 0; i < sizeof(value); ++i) { memory[base + rva + i] = data[i]; }
    }
    bool read(uintptr_t address, void* destination, size_t count) {
        ++reads;
        const auto data = (uint8_t*)destination;
        for (size_t i = 0; i < count; ++i) {
            const auto found = memory.find(address + i);
            if (found == memory.end()) { return false; }
            data[i] = found->second;
        }
        return true;
    }
    LayoutFixture(const native::VerifiedBuild& b, uintptr_t load_base = 0x140000000) : base(load_base) {
        timestamp = b.timestamp; image_size = b.layout.image_size;
        for (const auto& f : b.code) { bytes(f.rva, f.bytes); }
        bytes(b.code[4].rva + 0x1f, "483b0500000000");
        put(b.code[4].rva + 0x22, (int32_t)(b.layout.sentinel_rva - (b.code[4].rva + 0x26)));
        put(b.layout.sentinel_vtable_rva + 0x38, base + b.layout.empty_getter_rva);
        for (size_t i = 0; i < 3; ++i) { put(b.layout.bridge_vtable_rva + 8 * (i + 1), base + b.code[i + 8].rva); }
    }
    bool resolve(native::Layout& out) {
        return native::resolve_layout([&](uintptr_t a, void* p, size_t n) { return read(a, p, n); },
            base, timestamp, image_size, out);
    }
};
void native_layout_guards(const char* current_fixture) {
    for (const auto& build : native::verified_builds) {
        LayoutFixture f{build}; native::Layout out{};
        require(f.resolve(out) && out.context_rva == build.layout.context_rva && out.frame_rva == build.layout.frame_rva &&
            out.sentinel_rva == build.layout.sentinel_rva && out.bridge_vtable_rva == build.layout.bridge_vtable_rva &&
            out.camera_getter_rva == build.layout.camera_getter_rva, "Known native layout/RIP roots rejected");
        const auto before = f.memory;
        require(f.resolve(out) && before == f.memory, "Layout discovery wrote memory");
        const auto expect_rejection = [](LayoutFixture invalid) {
            native::Layout stale{0x140000000, 0x30000000, 1, 2, 3, 4, 5, 6, 7};
            require(!invalid.resolve(stale) && !stale.base && !stale.image_size && !stale.context_rva && !stale.frame_rva,
                "Rejected layout retained stale addresses");
        };
        for (const auto& fingerprint : build.code) {
            auto invalid = f; invalid.memory[invalid.base + fingerprint.rva] ^= 1;
            expect_rejection(invalid);
        }
        auto invalid = f;
        invalid.memory[invalid.base + build.code[4].rva + 0x22] ^= 1;
        expect_rejection(invalid); // Beyond the 32-byte next-node prefix: exercise RIP validation.
        for (size_t slot = 1; slot < 4; ++slot) {
            invalid = f; invalid.put(build.layout.bridge_vtable_rva + (uint32_t)slot * 8, invalid.base + build.code[8].rva);
            if (slot == 1) { invalid.put(build.layout.bridge_vtable_rva + 8, uintptr_t{0x700000000}); }
            expect_rejection(invalid);
        }
        invalid = f; invalid.put(build.layout.sentinel_vtable_rva + 0x38, f.base + build.layout.camera_getter_rva);
        expect_rejection(invalid);
        for (const auto& fingerprint : build.code) {
            invalid = f; invalid.memory.erase(invalid.base + fingerprint.rva + 31); expect_rejection(invalid);
        }
        invalid = f; invalid.timestamp++; invalid.reads = 0; expect_rejection(invalid);
        require(!invalid.resolve(out) && invalid.reads == 0, "Unknown identity performed native reads");
        invalid = f; invalid.image_size--; expect_rejection(invalid);
        invalid = f; invalid.timestamp = native::verified_builds[build.timestamp == native::verified_builds[0].timestamp ? 1 : 0].timestamp;
        expect_rejection(invalid);
        invalid = f; invalid.base = 0x00007ffffff00000; expect_rejection(invalid);
        LayoutFixture relocated{build, 0x7ff600000000};
        require(relocated.resolve(out) && out.base == relocated.base, "ASLR layout validation used preferred-base pointers");
    }
    require(current_fixture != nullptr, "Current EXE slices were not supplied");
    std::ifstream file(current_fixture); nlohmann::json captured; file >> captured;
    LayoutFixture actual{native::verified_builds[1]}; actual.memory.clear();
    actual.base = captured.at("base"); actual.timestamp = captured.at("timestamp"); actual.image_size = captured.at("image_size");
    for (const auto& block : captured.at("blocks")) {
        actual.bytes(block.at("rva"), block.at("bytes").get<std::string>());
    }
    native::Layout out{};
    require(actual.resolve(out) && out.context_rva == 0x132bf3e0 && out.frame_rva == 0x132cec10 &&
        out.sentinel_rva == 0x132bf9a8 && out.bridge_vtable_rva == 0x07a28668,
        "Current EXE fixture disagrees with implemented layout");
}
struct NativeFixture {
    native::Layout layout{};
    std::unordered_map<uintptr_t, uint8_t> memory;
    uintptr_t sentinel{0x200000}, context{0x220000}, interface{0x230000}, pcm{0x240000};
    std::vector<uintptr_t> nodes;
    template <typename T> void put(uintptr_t address, const T& value) {
        const auto bytes = (const uint8_t*)&value;
        for (size_t i = 0; i < sizeof(T); ++i) { memory[address + i] = bytes[i]; }
    }
    bool read(uintptr_t address, void* out, size_t size) {
        auto bytes = (uint8_t*)out;
        for (size_t i = 0; i < size; ++i) {
            const auto it = memory.find(address + i);
            if (it == memory.end()) { return false; }
            bytes[i] = it->second;
        }
        return true;
    }
    NativeFixture(size_t count = 3, size_t build = 0) {
        layout = native::verified_builds.at(build).layout;
        layout.base = 0x140000000;
        put(layout.base + layout.frame_rva, uint32_t{7});
        put(layout.base + layout.context_rva, context);
        put(layout.base + layout.sentinel_rva, sentinel);
        put(context + 0x10, interface);
        put(interface, layout.base + layout.bridge_vtable_rva);
        put(interface + 8, pcm); put(interface + 0x10, uintptr_t{0x250000});
        put(0x250000 + 0x228, uintptr_t{0x260000}); put(0x260000, layout.base + 0x5000);
        put(0x260000 + 0x208, 1.5f);
        put(interface + 0x140, std::array<uint8_t, 48>{});
        put(interface + 0x140, Vector{10, 30, 20});
        put(interface + 0x150, std::array<float, 4>{0, 0, 0, 1});
        put(interface + 0x160, 2 * std::atan(std::tan(0.2f) * 1.5f) * 57.29577951308232f);
        put(sentinel, layout.base + layout.sentinel_vtable_rva);
        put(sentinel + 8, uintptr_t{0x210000});
        for (size_t i = 0; i < count; ++i) { nodes.push_back(0x300000 + i * 0x10000); }
        put(0x210000, std::array<uintptr_t, 2>{nodes.empty() ? sentinel : nodes.back(), nodes.empty() ? sentinel : nodes.front()});
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto node = nodes[i], vt = layout.base + 0x10000 + i * 0x100;
            put(node, std::array<uintptr_t, 2>{vt, node + 0x1000});
            put(node + 0x1000, std::array<uintptr_t, 2>{i ? nodes[i - 1] : sentinel,
                i + 1 < nodes.size() ? nodes[i + 1] : sentinel});
            put(vt + 0x38, layout.base + layout.camera_getter_rva);
            put(node + 0x414, uint8_t{});
            put(node + 0x60, std::array<uint8_t, 64>{});
            put(node + 0x60, node + 0x2000);
            put(node + 0x70, std::array<float, 4>{1, 2, 3, 0});
            put(node + 0x80, std::array<float, 4>{101, 2, 3, 0});
            put(node + 0x2000 + 0x1c, 0.4f);
        }
    }
    NativeSource sample() {
        return native::sample([&](uintptr_t a, void* p, size_t n) { return read(a, p, n); }, layout, pcm, Recorder::clock_ns());
    }
};
void native_provider() {
    NativeFixture current{3, 1};
    require(valid_source(current.sample()) && current.sample().object == current.nodes[0],
        "Current build lost native list/bridge validation");
    NativeFixture f;
    const auto before = f.memory;
    auto s = f.sample();
    require(valid_source(s) && s.object == f.nodes[0] && s.frame == 7, "native fallback/identity lost");
    require(s.look_at == Vector{1010, 30, 20} && s.focus_distance == 1000, "native coordinates/focus changed");
    require(before == f.memory, "read-only provider wrote game memory");
    f.put(f.nodes[0] + 0x414, uint8_t{1}); f.put(f.nodes[2] + 0x414, uint8_t{1});
    require(f.sample().object == f.nodes[2], "native last nonzero selector not mirrored");
    f.put(f.layout.base + 0x10200 + 0x38, f.layout.base + f.layout.empty_getter_rva);
    require(f.sample().object == f.nodes[0], "empty getter not skipped");
    f.put(f.layout.base + 0x10200 + 0x38, f.layout.base + 0x7777);
    require(f.sample().status == SourceStatus::unsupported_node, "unknown getter adopted");
    f = NativeFixture{}; f.put(f.nodes.back() + 0x1008, f.nodes[0]);
    require(f.sample().status == SourceStatus::corrupt_list, "native cycle accepted");
    f = NativeFixture{65}; require(f.sample().status == SourceStatus::corrupt_list, "native walk unbounded");
    f = NativeFixture{}; f.put(f.nodes[0] + 0x80, Vector{NAN, 2, 3});
    require(f.sample().status == SourceStatus::invalid_values, "NaN target adopted");
    f = NativeFixture{}; f.put(f.interface + 0x140, Vector{999, 30, 20});
    require(f.sample().status == SourceStatus::bridge_mismatch, "unpublished source adopted");
    f = NativeFixture{}; f.memory.erase(f.nodes[0]);
    require(f.sample().status == SourceStatus::unreadable, "stale pointer adopted");
    f = NativeFixture{};
    auto source = native::sample([&](uintptr_t a, void* p, size_t n) { return f.read(a, p, n); }, f.layout, f.pcm + 8, Recorder::clock_ns());
    require(source.status == SourceStatus::manager_mismatch, "different PCM paired");
    int reads{};
    source = native::sample([&](uintptr_t a, void* p, size_t n) {
        if (a == f.layout.base + f.layout.frame_rva && ++reads > 1) { f.put(a, uint32_t{8}); }
        return f.read(a, p, n);
    }, f.layout, f.pcm, Recorder::clock_ns());
    require(source.status == SourceStatus::changed_during_read && source.object == 0, "torn source retained");
}
void target_framing_policies() {
    Camera c{};
    c.input = {{0, 0, 0}, {0, 0, 0}, 30, 1.5f, 0, true, true, false};
    c.camera_manager = 2;
    c.native_source.pose = c.input; c.native_source.look_at = {1000, 0, 0};
    c.native_source.focus_distance = 1000; c.native_source.camera_manager = 2;
    c.native_source.time_ns = Recorder::clock_ns(); c.native_source.status = SourceStatus::accepted;
    require(source_matches_input(c.native_source, c.input, 2, c.native_source.time_ns), "valid source pairing rejected");
    require(!source_matches_input(c.native_source, c.input, 2, c.native_source.time_ns + 250'000'001), "stale native source paired");
    require(!source_matches_input(c.native_source, c.input, 3, c.native_source.time_ns), "wrong native PCM paired");
    View v{}; v.input = v.neutral = v.output = c.input;
    v.neutral_valid = v.input_matches_assist = v.source_matches_input = true;
    require(view_suspects(c, v) == 0, "healthy original framing flagged");
    v.output.rotation[1] = 100;
    require(view_suspects(c, v) == 0 && target_framing(v.output, c.native_source.look_at).behind, "head motion blamed on assist");
    v.neutral.location = {1100, 0, 0};
    require((view_suspects(c, v) & (target_behind | dolly_past_target)) == (target_behind | dolly_past_target), "overshoot not retained");
    v.neutral = c.input; v.neutral.location[1] = 600;
    require(view_suspects(c, v) & target_offscreen, "neutral aim error not retained");
    v.source_matches_input = false;
    require(view_suspects(c, v) == 0, "unassociated target produced flags");
    v.source_matches_input = true; c.native_source.status = SourceStatus::unavailable;
    require(view_suspects(c, v) == 0, "missing provider changed legacy flags");
    auto roll = c.input; roll.rotation[2] = 90;
    const auto framing = target_framing(roll, Vector{1000, 10, 0});
    require(framing.valid && std::abs(framing.yaw_error) < .001f && framing.pitch_error > 0, "roll basis ignored");
}
void policies() {
    auto c = sample();
    require(camera_suspects(c) == 0, "healthy camera flagged");
    require(angle_delta(179, -179) == 2, "wrapped angle mismatch");
    auto after = c.input; after.rotation[1] = -179;
    require(!inferred_cut(c.input, after), "wrapped yaw produced a false cut");
    after.location[0] += 1000;
    require(inferred_cut(c.input, after), "short spatial cut lost");
    c.input.fov = NAN; require(camera_suspects(c) == invalid_camera, "NaN accepted");
    c = sample(); c.match_count = 2; c.mode = 2; c.source = 3;
    c.matches[0].focus_distance = 1000; c.matches[1].focus_distance = 4000;
    c.matches[0].confidence = .9f; c.matches[1].confidence = .85f;
    require(camera_suspects(c) & ambiguous_match, "ambiguous learned neighbours not flagged");
    c.mode = 3;
    require(camera_suspects(c) & ambiguous_match, "calibrated-framing ambiguity not flagged");
    c.safety_active = true; c.safety_up = 100; c.safety_min_z = 500; c.safety_predicted_z = 250;
    require((camera_suspects(c) & (large_safety_lift | estimated_floor_unmet)) == (large_safety_lift | estimated_floor_unmet), "safety markers lost");
    View v{}; v.neutral_valid = true; v.input = c.input; v.neutral.location[2] = 800;
    require(view_suspects(c, v) & safety_estimate_disagrees, "measured versus estimated lift not compared");
    std::array<char, 3> text{}; copy_text(text, "abcd"); require(std::string(text.data()) == "ab", "text not bounded");
    copy_text(text, {}); require(text[0] == 0, "empty marker text unsafe");
}
void sampling_policy() {
    constexpr uint64_t second = SamplingPolicy::interval_ns, base = 10 * second;
    for (const auto hz : {60, 120}) {
        SamplingPolicy policy;
        unsigned captures{};
        for (int i = 0; i <= hz * 60; ++i) {
            auto pose = sample().input;
            pose.location[0] += (float)i * 2000.0f / hz;
            pose.rotation[1] += (float)i * 200.0f / hz;
            const auto decision = policy.observe(pose, 1, 2, 0, base + (uint64_t)i * second / hz, Detail::light);
            require(!decision.inferred_cut, "smooth motion across distant samples became a cut");
            captures += decision.capture;
        }
        require(captures == 61, "normal capture rate is not one sample per second");
    }
    SamplingPolicy policy;
    auto pose = sample().input;
    unsigned samples[3]{}, cuts{};
    for (int i = 0; i < 6 * 120; ++i) {
        const auto shot = i < 270 ? 0 : i < 510 ? 1 : 2;
        pose.location[0] = 100.0f + 5000.0f * shot;
        const auto decision = policy.observe(pose, 1, 2, 0, base + (uint64_t)i * second / 120, Detail::light);
        samples[shot] += decision.capture;
        cuts += decision.inferred_cut;
        if (i == 270 || i == 510) { require(decision.capture && decision.inferred_cut, "two-second cut edge was missed"); }
    }
    require(samples[0] >= 2 && samples[1] >= 2 && samples[2] >= 1 && cuts == 2, "brief shot coverage lost");

    policy.reset();
    unsigned noisy{};
    for (int i = 0; i < 120; ++i) {
        pose.location[0] = i % 2 ? 10000.0f : 100.0f;
        noisy += policy.observe(pose, 1, 2, 0, base + (uint64_t)i * second / 120, Detail::light).capture;
    }
    require(noisy == 4, "noisy cuts caused an unbounded probe burst");
    require(policy.observe(pose, 1, 2, 0, base + second, Detail::light).inferred_cut, "pending cut edge was forgotten");
    require(policy.observe(pose, 1, 2, 0, base + second + 1, Detail::light, true).capture, "manual capture was delayed");
    require(policy.observe(pose, 3, 2, 0, base + second + 2, Detail::light).capture, "world change was delayed");
    require(policy.observe(pose, 3, 4, 0, base + second + 3, Detail::light).capture, "PCM change was delayed");
    require(policy.observe(pose, 3, 4, 2, base + second + 4, Detail::light).capture, "mode change was delayed");
    require(policy.observe(pose, 3, 4, 2, base, Detail::light).capture, "reversed clock underflowed");
    pose.fov = NAN;
    require(!policy.observe(pose, 3, 4, 2, base + 1, Detail::light).capture, "invalid pose captured");
    pose = sample().input;
    require(policy.observe(pose, 3, 4, 2, base + 2, Detail::light).capture, "invalid-pose recovery was delayed");
    require(!policy.observe(pose, 0, 4, 2, base + 3, Detail::light).capture, "missing world accepted");
    require(!policy.observe(pose, 3, 0, 2, base + 3, Detail::light).capture, "missing PCM accepted");
    require(!policy.observe(pose, 3, 4, 2, 0, Detail::light).capture, "missing timestamp accepted");
    const auto end = std::numeric_limits<uint64_t>::max();
    require(policy.observe(pose, 1, 2, 0, end - second, Detail::light).capture, "large clock start failed");
    require(policy.observe(pose, 1, 2, 0, end, Detail::light).capture, "large clock subtraction failed");
    policy.reset();
    for (int i = 0; i < 240; ++i) {
        require(policy.observe(pose, 1, 2, 0, base + i, Detail::detailed).capture, "detailed capture was decimated");
    }
}
void light_recording() {
    Fixture f; Recorder r;
    require(r.start(f.root, "{}"), "light start failed");
    require(r.detail() == Detail::light, "light is not the default");
    auto c = sample();
    r.camera(c, 1);
    const auto first = r.latest_camera();
    require(first.has_value(), "initial light camera absent");
    for (int i = 0; i < 240; ++i) {
        r.camera(c, 1);
        for (int eye = 0; eye < 2; ++eye) {
            auto t = r.begin_view(c.input, eye, eye, 1);
            if (t) {
                t.event.view.neutral = t.event.view.output = c.input;
                t.event.view.neutral_valid = true;
                r.finish_view(t);
            }
            r.projection(Matrix{}, eye, 1);
            r.projection(Matrix{}, eye, 1);
        }
        r.observation(Observation{}, 1);
    }
    require(r.latest_camera()->camera_sequence == first->camera_sequence, "routine updates generated full camera records");
    auto other = c.input; other.location[0] += 400;
    auto auxiliary = r.begin_view(other, 0, 0, 1);
    require(auxiliary && !auxiliary.event.view.input_matches_assist, "auxiliary channel masked the matched eye");
    r.finish_view(auxiliary); r.projection(Matrix{}, 0, 1); r.projection(Matrix{}, 0, 1);
    require(!r.begin_view(other, 0, 0, 1), "auxiliary channel was unbounded");
    r.mark(Marker::bad, "short bad shot", 1);
    require(r.take_capture_request() && !r.take_capture_request(), "manual force was not an edge");
    r.mark(Marker::good, "short good shot", 1);
    r.camera(c, 1);
    const auto marked = r.latest_camera();
    require(marked && marked->camera_sequence != first->camera_sequence, "manual mark did not refresh the sample");
    auto next = r.begin_view(c.input, 0, 0, 1);
    require((bool)next, "fresh marker sample did not admit eye data");
    r.finish_view(next); r.projection(Matrix{}, 0, 1);
    r.mark(Marker::automatic, "automatic flag", 1);
    require(!r.take_capture_request(), "automatic marks created a probe loop");
    std::this_thread::sleep_for(std::chrono::milliseconds{260});
    require(!r.begin_view(c.input, 1, 1, 1), "stale light snapshot fabricated eye data");
    c.rendering_method = 2; r.camera(c, 1);
    require(!r.latest_neutral(), "mode change retained a neutral snapshot");
    r.stop(); settle(r);
    unsigned cameras{}, views{}, projections{}, observations{}, marks{};
    for (const auto& e : f.events()) {
        const auto kind = e["kind"];
        cameras += kind == "camera"; views += kind == "view"; projections += kind == "projection";
        observations += kind == "observation"; marks += kind == "marker";
        if (kind == "projection" || kind == "view") {
            require(e["camera_sequence"] == first->camera_sequence || e["camera_sequence"] == marked->camera_sequence,
                "linked eye data drifted from its camera");
        }
    }
    require(cameras == 3 && views == 4 && projections == 4 && observations == 1 && marks == 3,
        "light session lost a marker or multiplied duplicate events");
    for (const auto& directory : std::filesystem::directory_iterator(f.root)) {
        std::ifstream file(directory.path() / "metadata.json"); nlohmann::json meta; file >> meta;
        require(meta["schema"] == 2 && meta["sampling"] == "light" && meta["limits"]["normal_hz"] == 1 &&
            meta["limits"]["burst_hz"] == 0, "light metadata describes high-rate capture");
    }
    require(r.start(f.root, "{}", Detail::detailed), "detailed restart failed");
    require(r.detail() == Detail::detailed, "detailed option ignored");
    r.camera(c, 1); r.finish_view(next); r.projection(Matrix{}, 0, 1);
    require(!r.latest_neutral(), "restart reused light tickets");
    r.stop(); settle(r);
}
void disabled() {
    Fixture f; Recorder r;
    const auto c = sample(); const auto before = c;
    r.camera(c, 1); r.mark(Marker::bad, "ignored", 1); r.invalidate(1, "ignored", 1);
    require(!r.begin_view(c.input, 0, 0, 1), "disabled view trace active");
    require(std::memcmp(&c, &before, sizeof(c)) == 0, "observer altered input camera");
    require(r.counters().written == 0 && !r.latest_camera(), "disabled recorder retained data");
    require(std::filesystem::is_empty(f.root), "disabled trace wrote files");
}
void association_and_restart() {
    Fixture f; Recorder r;
    require(r.start(f.root, "{}", Detail::detailed), "start failed");
    auto c = sample(); r.camera(c, 1);
    auto ticket = r.begin_view(c.input, 0, 0, 1); require((bool)ticket, "no camera ticket");
    const auto camera_sequence = ticket.event.camera_sequence;
    c.focus_after = 4000; r.camera(c, 1);
    ticket.event.view.neutral = ticket.event.view.input; ticket.event.view.neutral_valid = true;
    ticket.event.view.output = ticket.event.view.neutral;
    r.finish_view(ticket); r.projection(Matrix{}, 0, 1);
    r.mark(Marker::bad, "quick cut", 1);
    auto stale = r.begin_view(c.input, 0, 0, 1);
    r.invalidate(0, "travel", 1); r.finish_view(stale);
    c.world = 3; r.camera(c, 1);
    require(!r.latest_neutral(), "travel reused a neutral pose");
    r.stop(); settle(r);
    const auto events = f.events();
    bool view{}, matrix{}, marker{};
    for (const auto& e : events) {
        if (e["kind"] == "view") {
            require(e["camera_sequence"] == camera_sequence, "view used a newer assist transaction");
            require(e["assist"]["focus_after"] == 1800, "view snapshot stitched from different updates"); view = true;
        }
        if (e["kind"] == "projection") { matrix = true; require(e["camera_sequence"] == camera_sequence, "projection association drifted"); }
        if (e["kind"] == "marker") { marker = true; }
    }
    require(view && matrix && marker, "cut context or marker not persisted");
    require(r.start(f.root, "{}"), "restart failed");
    r.camera(sample(), 1); r.finish_view(ticket); r.projection(Matrix{}, 0, 1);
    require(!r.latest_neutral(), "restart accepted a prior session ticket");
    r.stop(); settle(r);
}
void cut_observation_association() {
    Fixture f; Recorder r;
    require(r.start(f.root, "{}", Detail::detailed), "association start failed");
    auto c = sample(); r.camera(c, 1);
    auto ticket = r.begin_view(c.input, 0, 0, 1); require((bool)ticket, "association ticket absent");
    const auto cut = ticket.event.cut_sequence, camera = ticket.event.camera_sequence;
    ticket.event.view.neutral = ticket.event.view.output = ticket.event.view.input;
    ticket.event.view.neutral_valid = true;
    c.input.location[0] += 1000; r.camera(c, 1);
    r.finish_view(ticket); r.projection(Matrix{}, 0, 1);
    Observation observation{}; observation.reference_camera = camera;
    r.observation(observation, 1);
    r.mark(Marker::bad, "retain", 1);
    r.stop(); settle(r);
    int bound_events{};
    for (const auto& e : f.events()) {
        if (e["kind"] == "view" || e["kind"] == "projection" || e["kind"] == "observation") {
            require(e["camera_sequence"] == camera && e["cut_sequence"] == cut, "older transaction relabelled as new cut");
            require(e["assist"]["input"]["location"][0] == 100, "observation camera data mixed across cuts");
            ++bound_events;
        }
    }
    require(bound_events == 3, "older valid transaction context lost");
}
void native_trace_serialization() {
    Fixture f; NativeFixture provider; Recorder r;
    require(r.start(f.root, "{}"), "native trace start failed");
    auto c = sample();
    c.native_source = provider.sample(); c.input = c.native_source.pose; c.camera_manager = provider.pcm;
    c.native_focus_guard_enabled = c.native_focus_guard_applied = true;
    c.native_focus_guard_status = 9; c.native_focus_guard_dolly_before = 2800;
    c.native_focus_guard_lift_before = 211; c.native_focus_guard_end_z = 178; c.native_focus_guard_depth = 75;
    c.native_focus_guard_floor = 170; c.native_focus_guard_zone = 1; c.native_focus_guard_protected = false;
    r.camera(c, 1);
    auto ticket = r.begin_view(c.input, 0, 0, 1);
    require(ticket && ticket.event.view.source_matches_input, "native source not associated with stereo input");
    auto& view = ticket.event.view;
    view.neutral = view.output = c.input; view.neutral_valid = true;
    view.hmd_pose_valid = true; view.hmd_rotation = {0, 0, 0, 1}; view.head_translation = {1, 2, 3};
    view.head_translation_applied = view.hmd_rotation_applied = true;
    view.assist_offset_status = 3;
    view.output.rotation[1] = 100;
    r.finish_view(ticket); r.mark(Marker::good, "keep context", 1);
    r.stop(); settle(r);
    bool found{};
    for (const auto& e : f.events()) {
        if (e["kind"] != "view") { continue; }
        require(e["assist"]["native_source"]["look_at"] == nlohmann::json::array({1010, 30, 20}), "native target not serialized");
        require(!e["assist"]["native_source"]["subject_identity_verified"].get<bool>(), "look-at mislabeled as subject");
        const auto& guard = e["assist"]["native_focus_guard"];
        require(guard["enabled"].get<bool>() && guard["applied"].get<bool>() && guard["status"] == 9 &&
            guard["dolly_before"] == 2800 && guard["lift_before"] == 211 && guard["end_z"] == 178 && guard["target_depth"] == 75 &&
            guard["floor"] == 170 && guard["zone"] == 1 && !guard["protected"].get<bool>(),
            "Focus-guard decision not associated with its camera/view");
        require(e["view"]["hmd"]["head_translation_cm"] == nlohmann::json::array({1, 2, 3}), "HMD offsets lost");
        require(e["view"]["assist_offset_status"] == 3, "Cut-offset rejection status lost");
        require(e["view"]["target_framing"]["hmd"]["behind"].get<bool>() &&
            !e["view"]["target_framing"]["neutral"]["behind"].get<bool>(), "head/assist framing mixed");
        require(e["suspects"] == 0, "headset-only movement became a bad-cut flag");
        found = true;
    }
    require(found, "native view context lost");
}
void failures_and_contention() {
    Fixture f;
    {
        Recorder r{{1, 32, 8192}};
        require(r.start(f.root, "{}"), "bounded start failed");
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] { for (int j = 0; j < 1000; ++j) { r.camera(sample(), 1); r.mark(Marker::bad, "overflow", 1); } });
        }
        for (auto& thread : threads) { thread.join(); }
        for (int i = 0; i < 100 && r.active(); ++i) {
            r.mark(Marker::bad, "storage-boundary", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        r.stop(); settle(r);
        require(r.counters().dropped > 0, "contention not reported");
        require(r.counters().bytes <= 8192, "storage cap exceeded");
        require(r.counters().status == Status::storage_limit, "storage exhaustion not isolated");
    }
    {
        const auto file = f.root / "not-a-directory"; std::ofstream(file) << "test";
        Recorder r; require(r.start(file, "{}"), "asynchronous I/O start failed");
        for (int i = 0; i < 100 && r.active(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds{10}); }
        require(!r.active() && r.counters().status == Status::io_error, "I/O failure did not stop only the trace");
    }
}
}
int main(int argc, char** argv) {
    try { native_layout_guards(argc == 2 ? argv[1] : nullptr); policies(); sampling_policy(); light_recording(); native_provider(); target_framing_policies(); disabled(); association_and_restart(); cut_observation_association(); native_trace_serialization(); failures_and_contention(); std::cout << "ProSpi camera trace tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
