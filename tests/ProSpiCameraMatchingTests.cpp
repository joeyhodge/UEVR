#include "mods/vr/ProSpiCameraMatching.hpp"
#include "mods/vr/ProSpiCameraReturnSmoothing.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
namespace matching = uevr::prospi::matching;
namespace smoothing = uevr::prospi::return_smoothing;

void require(bool ok, const char* message) {
    if (!ok) { throw std::runtime_error(message); }
}

struct Calibration {
    std::string camera_id;
    float dolly_distance{}, actual_min_fov{}, projection_multiplier{1};
    float score{};
    int generation{};
    bool has_pose{true};
    static inline std::atomic<size_t> copies{};
    static inline bool fail_copy{};

    Calibration() = default;
    Calibration(const Calibration& other)
        : camera_id(other.camera_id), dolly_distance(other.dolly_distance), actual_min_fov(other.actual_min_fov),
          projection_multiplier(other.projection_multiplier), score(other.score), generation(other.generation), has_pose(other.has_pose) {
        ++copies;
        if (fail_copy) { throw std::bad_alloc{}; }
    }
    Calibration(Calibration&&) = default;
    Calibration& operator=(const Calibration&) = default;
    Calibration& operator=(Calibration&&) = default;
};
using Map = std::unordered_map<std::string, Calibration>;
using Candidate = matching::Candidate<Calibration>;
struct OldCandidate { Calibration calibration; float score{}, confidence{}; };

Map make_map(size_t count, int ties) {
    Map out;
    std::mt19937 random{0x5417};
    for (size_t i = 0; i < count; ++i) {
        Calibration c;
        c.camera_id = "camera-with-a-non-small-string-name/" + std::to_string(i);
        c.score = ties == 2 ? .25f : ties == 1 ? (random() % 4) * .25f : (random() % 10000) * .001f;
        c.dolly_distance = 10.0f + (random() % 20000);
        c.actual_min_fov = 5.0f + (random() % 6000) * .01f;
        c.projection_multiplier = .1f + (random() % 2900) * .001f;
        c.has_pose = i % 7 != 0;
        const auto key = c.camera_id;
        out.emplace(key, std::move(c));
    }
    return out;
}

void fill(std::vector<Candidate>& out, const auto& entries, float minimum) {
    for (const auto& c : entries) {
        const auto confidence = 1.0f / (1.0f + c.score);
        if (c.has_pose && confidence >= minimum) { out.push_back({&c, c.score, confidence}); }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
}

void snapshot_and_order() {
    matching::PublishedCalibrations<Calibration> published;
    require(!published.read(), "Unpublished snapshot was not empty");
    matching::Workspace<Candidate> workspace;
    for (const auto count : {0u, 1u, 2u, 3u, 8u, 16u, 17u, 31u, 32u, 33u, 128u, 1024u}) {
        for (int ties = 0; ties < 3; ++ties) {
            auto map = make_map(count, ties);
            require(published.publish(map), "Snapshot publication failed");
            auto snapshot = published.read();
            require(snapshot && snapshot->size() == map.size(), "Snapshot truncated calibrations");
            size_t index{};
            for (const auto& [id, c] : map) {
                require((*snapshot)[index++].camera_id == c.camera_id, "Map iteration order changed");
            }
            for (const float minimum : {.05f, .5f, .95f}) {
                std::vector<OldCandidate> reference;
                for (const auto& [id, c] : map) {
                    const auto confidence = 1.0f / (1.0f + c.score);
                    if (c.has_pose && confidence >= minimum) { reference.push_back({c, c.score, confidence}); }
                }
                std::sort(reference.begin(), reference.end(), [](const auto& a, const auto& b) { return a.score < b.score; });
                auto lease = workspace.acquire(snapshot->size());
                auto& candidates = lease.values();
                fill(candidates, *snapshot, minimum);
                require(candidates.size() == reference.size(), "Match count changed");
                std::array<float, 4> old_blend{}, new_blend{};
                for (size_t i = 0; i < candidates.size(); ++i) {
                    const auto& a = candidates[i];
                    const auto& b = reference[i];
                    require(a.calibration->camera_id == b.calibration.camera_id && a.score == b.score &&
                        a.confidence == b.confidence, "Full-sort or equal-score order changed");
                    if (i < 3) {
                        const auto aw = a.confidence * a.confidence, bw = b.confidence * b.confidence;
                        new_blend[0] += aw; old_blend[0] += bw;
                        new_blend[1] += a.calibration->dolly_distance * aw; old_blend[1] += b.calibration.dolly_distance * bw;
                        new_blend[2] += a.calibration->actual_min_fov * aw; old_blend[2] += b.calibration.actual_min_fov * bw;
                        new_blend[3] += a.calibration->projection_multiplier * aw; old_blend[3] += b.calibration.projection_multiplier * bw;
                    }
                }
                require(new_blend == old_blend, "Top-three weighted camera blend changed");
            }
        }
    }

    auto map = make_map(32, 0);
    require(published.publish(map), "Initial edit publication failed");
    auto old = published.read();
    const auto removed_id = old->front().camera_id;
    map.erase(removed_id);
    for (auto& [id, c] : map) { c.generation = 7; }
    require(published.publish(map), "Edit/erase publication failed");
    for (const auto& c : *published.read()) {
        require(c.camera_id != removed_id && c.generation == 7, "Snapshot reused an edited/deleted calibration");
    }
    require(old->size() == 32 && old->front().generation == 0, "Edit invalidated an in-flight reader");
    Calibration::fail_copy = true;
    const auto succeeded = published.publish(map);
    Calibration::fail_copy = false;
    require(!succeeded && !published.read(), "Failed rebuild exposed stale entries");
    require(old->front().camera_id == removed_id, "Failed rebuild invalidated retained snapshot");
    map = make_map(128, 1);
    require(published.publish(map) && published.read()->size() == 128, "Reload did not replace snapshot");
    map.clear();
    require(published.publish(map) && published.read()->empty(), "Clear did not publish empty snapshot");
}

void workspace_and_publication() {
    auto map = make_map(1024, 1);
    matching::PublishedCalibrations<Calibration> published;
    require(published.publish(map), "Workspace fixture publication failed");
    auto snapshot = published.read();
    matching::Workspace<Candidate> workspace;
    const Candidate* storage{};
    {
        auto lease = workspace.acquire(snapshot->size());
        storage = lease.values().data();
    }
    const auto copies_before = Calibration::copies.load();
    for (int frame = 0; frame < 1000; ++frame) {
        auto lease = workspace.acquire(snapshot->size());
        auto& values = lease.values();
        require(values.empty() && values.data() == storage, "Steady-state candidate storage was not reused");
        fill(values, *snapshot, .05f);
        const auto first = values.front().calibration;
        if (frame == 0) {
            auto nested = workspace.acquire(snapshot->size() * 2);
            require(nested.values().empty() && nested.values().data() != storage, "Nested update reused outer storage");
            fill(nested.values(), *snapshot, .05f);
            require(values.front().calibration == first && values.data() == storage, "Nested update corrupted outer candidates");
        }
    }
    require(Calibration::copies.load() == copies_before, "Steady-state matching copied calibrations/strings");
    bool failed{};
    try { auto lease = workspace.acquire(std::numeric_limits<size_t>::max()); }
    catch (const std::exception&) { failed = true; }
    require(failed, "Impossible workspace reservation did not fail");
    { auto lease = workspace.acquire(snapshot->size()); require(lease.values().data() == storage, "Failed reservation left workspace leased"); }
    try {
        auto lease = workspace.acquire(snapshot->size());
        fill(lease.values(), *snapshot, .05f);
        throw std::runtime_error("exercise scope unwind");
    } catch (const std::runtime_error&) {}
    { auto lease = workspace.acquire(snapshot->size()); require(lease.values().empty() && lease.values().data() == storage, "Exception did not release workspace"); }

    map = make_map(8, 0);
    require(published.publish(map), "Concurrent fixture publication failed");
    std::atomic<bool> stop{}, coherent{true};
    std::atomic<unsigned> ready{}, reads{};
    const auto reader = [&] {
        ++ready;
        do {
            auto s = published.read();
            if (!s || s->size() != 8) { coherent = false; break; }
            for (const auto& c : *s) { if (c.generation != s->front().generation) { coherent = false; } }
            ++reads;
        } while (!stop.load());
    };
    std::thread a{reader}, b{reader};
    while (ready.load() != 2) { std::this_thread::yield(); }
    for (int generation = 1; generation <= 1000; ++generation) {
        for (auto& [id, c] : map) { c.generation = generation; }
        if (!published.publish(map)) { coherent = false; break; }
    }
    stop = true;
    a.join(); b.join();
    require(coherent && reads > 0, "Concurrent reader observed a partial or recycled snapshot");
    std::cout << "ProSpi matching: full-sort/tie/weighted equivalence; 1000 updates reuse storage with zero calibration copies\n";
}

constexpr smoothing::Context context{0x1000, 0x2000, 0, 3, 2, 1, false, false};

float response(const std::vector<double>& steps, float setting) {
    smoothing::Timeline timeline;
    double now = 100;
    timeline.update(context, now, false, setting);
    float previous = 1000;
    for (const auto dt : steps) {
        now += dt;
        previous = smoothing::apply(10000, previous, timeline.update(context, now, false, setting));
    }
    return previous;
}

void time_based_return() {
    for (const auto setting : {0.0f, .25f, .75f, .95f}) {
        const auto reference = response(std::vector<double>(30, 1.0 / 30), setting);
        for (const auto hz : {15, 30, 60, 90, 120, 144}) {
            require(std::abs(response(std::vector<double>(hz, 1.0 / hz), setting) - reference) < .03f,
                "Dolly return response changed with update rate");
        }
        std::vector<double> variable(30, 1.0 / 60);
        variable.insert(variable.end(), 60, 1.0 / 120);
        require(std::abs(response(variable, setting) - reference) < .03f, "Variable-rate dolly response changed");
    }
    smoothing::Timeline timeline;
    require(timeline.update(context, 100, false, .25f) == 0, "First observation carried history");
    auto retention = timeline.update(context, 100 + 1.0 / 30, false, .25f);
    require(std::abs(retention - .25f) < 1e-6f && smoothing::apply(5000, 1000, retention) == 4000,
        "30 Hz reference did not preserve previous blend");
    require(smoothing::apply(1000, 5000, retention) == 1000, "Dolly decrease was delayed");
    require(smoothing::apply(5000, 1000, 0) == 5000 && smoothing::apply(5000, 1000, 1) == 1000,
        "Retention endpoints were not respected");

    const auto reset_case = [&](smoothing::Context changed) {
        smoothing::Timeline clock;
        clock.update(context, 100, false, .95f);
        require(clock.update(changed, 100.02, false, .95f) == 0, "Identity/mode change carried smoothing");
    };
    for (const auto member : {&smoothing::Context::world, &smoothing::Context::camera_manager}) {
        auto changed = context; changed.*member += 1; reset_case(changed);
        changed.*member = 0; reset_case(changed);
    }
    for (const auto member : {&smoothing::Context::rendering_method, &smoothing::Context::sequencer_mode,
                             &smoothing::Context::play_mode, &smoothing::Context::zone}) {
        auto changed = context; changed.*member += 1; reset_case(changed);
    }
    for (const auto member : {&smoothing::Context::exact_override, &smoothing::Context::cinematic_assist}) {
        auto changed = context; changed.*member = true; reset_case(changed);
    }
    for (const auto bad_time : {100.0, 99.0, 101.0, 0.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
        smoothing::Timeline clock;
        clock.update(context, 100, false, .95f);
        require(clock.update(context, bad_time, false, .95f) == 0, "Invalid clock/gap retained history");
    }
    timeline.update(context, 101, false, .95f);
    require(timeline.update(context, 101.02, true, .95f) == 0, "Hard cut was smoothed");
    require(timeline.update(context, 101.04, false, .95f) > 0, "New cut could not establish continuity");
    timeline.reset();
    require(timeline.update(context, 101.06, false, .95f) == 0, "Disabled/unavailable path kept history");
    require(timeline.update(context, 101.08, false, 0) == 0, "Zero smoothing retained history");
    for (const auto bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        require(timeline.update(context, 101.1, false, bad) == 0, "Non-finite smoothing retained history");
        require(smoothing::apply(5000, 1000, bad) == 5000 && smoothing::apply(5000, bad, .5f) == 5000,
            "Invalid history poisoned dolly return");
    }
    std::cout << "ProSpi dolly return: 15-144 Hz/variable-rate equivalence and cut/identity/gap resets passed\n";
}
} // namespace

void run_prospi_camera_matching_tests() {
    snapshot_and_order();
    workspace_and_publication();
    time_based_return();
}
