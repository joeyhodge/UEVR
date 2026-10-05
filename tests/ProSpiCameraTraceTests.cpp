#include "mods/vr/ProSpiCameraTrace.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

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
    try { policies(); disabled(); association_and_restart(); failures_and_contention(); std::cout << "ProSpi camera trace tests passed\n"; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
