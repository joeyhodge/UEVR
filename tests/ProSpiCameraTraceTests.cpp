#include "mods/vr/ProSpiCameraTrace.hpp"
#include "mods/vr/ProSpiNativeCameraSource.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
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
struct NativeFixture {
    native::Layout layout{0x140000000, 0x30000000};
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
    NativeFixture(size_t count = 3) {
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
    require(r.start(f.root, "{}"), "start failed");
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
    require(r.start(f.root, "{}"), "association start failed");
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
int main() {
    try { policies(); native_provider(); target_framing_policies(); disabled(); association_and_restart(); cut_observation_association(); native_trace_serialization(); failures_and_contention(); std::cout << "ProSpi camera trace tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
