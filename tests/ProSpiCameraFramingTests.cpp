#include "mods/vr/ProSpiCameraFraming.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace uevr::prospi::framing;
using Json = nlohmann::json;
namespace {
void require(bool ok, const std::string& message) {
    if (!ok) { throw std::runtime_error(message); }
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(input.good(), "Could not open " + path.string());
    return Json::parse(input);
}
Pose read_pose(const Json& p) {
    return {p.at("location").get<Vector>(), p.at("rotation").get<Vector>(), p.at("fov").get<float>()};
}
Request read_request(const Json& r) {
    return {
        .camera = read_pose(r.at("camera")), .mode = r.at("mode"),
        .prospi = r.at("prospi"), .dolly_enabled = r.at("dolly_enabled"),
        .sequencer_enabled = r.at("sequencer_enabled"), .safety_enabled = r.at("safety_enabled"),
        .exact_override = r.at("exact_override"), .learned = r.at("learned"),
        .eligible_family = r.at("eligible_family"), .protected_risk = r.at("protected_risk"),
        .focus = r.at("focus"), .legacy_ceiling = r.at("legacy_ceiling"),
        .effective_fov = r.at("effective_fov"), .base_fov = r.at("base_fov"),
        .forward_offset = r.at("forward_offset"), .right_offset = r.at("right_offset"),
        .up_offset = r.at("up_offset"), .max_safety_up = r.at("max_safety_up"),
        .required_floor = r.at("required_floor"),
    };
}
Calibration read_calibration(const Json& c) {
    return {read_pose(c.at("pose")), c.at("focus"), c.at("min_fov"), c.at("multiplier"),
        c.at("confidence"), c.at("center_field_family")};
}
std::vector<Calibration> read_matches(const Json& list) {
    std::vector<Calibration> out;
    for (const auto& c : list) { out.push_back(read_calibration(c)); }
    return out;
}
void captured_fixtures(const Json& fixtures) {
    size_t accepted{};
    for (const auto& f : fixtures.at("fixtures")) {
        auto r = read_request(f.at("request"));
        const auto matches = read_matches(f.at("matches"));
        const auto d = evaluate(r, matches);
        const auto name = f.at("name").get<std::string>();
        require(d.accepted() == f.at("expected_accepted").get<bool>(), name + ": " + status_name(d.status));
        if (d.accepted()) {
            ++accepted;
            require(d.learned_focus > f.at("recorded_focus").get<float>(), name + ": no framing improvement proposed");
            require(d.proposed_dolly > 0 && d.proposed_dolly < d.learned_focus, name + ": dolly overshot focus");
        }
        // The helper must have no effect on every historical mode, including overlapping good cuts.
        for (int mode = 0; mode <= 2; ++mode) {
            r.mode = mode;
            const auto legacy = evaluate(r, matches);
            require(legacy.status == Status::not_selected && legacy.learned_focus == 0 && legacy.proposed_dolly == 0,
                name + ": old mode changed");
        }
    }
    require(accepted != 0 && accepted != fixtures.at("fixtures").size(), "Fixtures need accepted and fallback cameras");
    std::cout << "Captured Tokyo Dome fixtures: " << fixtures.at("fixtures").size() << ", accepted=" << accepted << '\n';
}
void guards(const Json& fixtures) {
    const auto found = std::find_if(fixtures.at("fixtures").begin(), fixtures.at("fixtures").end(), [](const Json& sample) {
        return sample.at("name").get<std::string>() == "bad_behind_pitcher_0";
    });
    require(found != fixtures.at("fixtures").end(), "Missing guard fixture");
    const auto& f = *found;
    const auto base = read_request(f.at("request"));
    const auto matches = read_matches(f.at("matches"));
    require(evaluate(base, matches).accepted(), "Invalid guard fixture");
    auto reject = [&](Request r, const char* name) { require(!evaluate(r, matches).accepted(), name); };
    for (auto field : {&Request::prospi, &Request::dolly_enabled, &Request::sequencer_enabled,
                      &Request::safety_enabled, &Request::learned, &Request::eligible_family}) {
        auto r = base; r.*field = false; reject(r, "Capability gate bypassed");
    }
    auto r = base; r.exact_override = true;
    require(evaluate(r, matches).status == Status::exact_override, "Exact override changed");
    r = base; r.protected_risk = true; reject(r, "Protected risk changed");
    r = base; r.camera.location[0] = 6710; reject(r, "Stand rig accepted");
    r = base; r.camera.location[1] = 500; reject(r, "Behind-plate rig accepted");
    r = base; r.camera.location[2] = 0; reject(r, "Under-ground rig accepted");
    r = base; r.camera.rotation[1] = -90; reject(r, "Backward rig accepted");
    r = base; r.camera.rotation[0] = -50; reject(r, "Steep rig accepted");
    r = base; r.camera.fov = 90; reject(r, "Wide shot accepted");
    r = base; r.mode = 4; reject(r, "Unknown mode accepted");
    r = base; r.focus += 20; reject(r, "Post-match override bypassed");
    r = base; r.base_fov = r.effective_fov; reject(r, "Non-forward dolly accepted");
    r = base; r.forward_offset = 10000; reject(r, "Home-plate overshoot accepted");
    r = base; r.right_offset = 3000; reject(r, "Unbounded lateral offset accepted");
    r = base; r.max_safety_up = 4000; reject(r, "Unbounded lifted framing accepted");
    r = base; r.max_safety_up = 0; reject(r, "Insufficient floor clearance accepted");
    require(!evaluate(base, {}).accepted(), "Absent support accepted");
    require(!evaluate(base, {matches.data(), 1}).accepted(), "Single automatic match accepted");
    auto candidates = matches; candidates.push_back(matches.front());
    require(!evaluate(base, candidates).accepted(), "Oversized support accepted");
    candidates = matches; candidates[0].focus = 20000;
    require(evaluate(base, candidates).status == Status::conflicting_focus, "Conflicting matches accepted");
    candidates = matches; candidates[0].confidence = .79f;
    require(evaluate(base, candidates).status == Status::weak_match, "Weak match accepted");
    candidates = matches; candidates[0].pose.location[0] = 2000;
    require(evaluate(base, candidates).status == Status::weak_match, "Distant match accepted");
    candidates = matches; candidates[0].center_field_family = false;
    require(!evaluate(base, candidates).accepted(), "Different preset family accepted");
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()}) {
        for (auto field : {&Request::focus, &Request::legacy_ceiling, &Request::effective_fov, &Request::base_fov,
                          &Request::forward_offset, &Request::right_offset, &Request::up_offset, &Request::max_safety_up,
                          &Request::required_floor}) {
            r = base; r.*field = bad; reject(r, "Non-finite input accepted");
        }
        for (auto field : {&Calibration::focus, &Calibration::min_fov, &Calibration::multiplier, &Calibration::confidence}) {
            candidates = matches; candidates[0].*field = bad;
            require(!evaluate(base, candidates).accepted(), "Non-finite calibration accepted");
        }
        for (size_t axis = 0; axis < 3; ++axis) {
            r = base; r.camera.location[axis] = bad; reject(r, "Non-finite position accepted");
            r = base; r.camera.rotation[axis] = bad; reject(r, "Non-finite rotation accepted");
            candidates = matches; candidates[0].pose.location[axis] = bad;
            require(!evaluate(base, candidates).accepted(), "Non-finite saved position accepted");
        }
    }
    r = base; r.camera.rotation[1] += 360;
    require(evaluate(r, matches).accepted(), "Wrapped yaw rejected");
    candidates = matches; std::reverse(candidates.begin(), candidates.end());
    require(std::abs(evaluate(base, candidates).learned_focus - evaluate(base, matches).learned_focus) < .01f,
        "Support order changed the blend");
}

// Offline shadow review consumes the trace's saved evidence, never a live object or profile setting.
int shadow(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto meta = read_json(root / "metadata.json");
    const auto& saved = meta.at("calibration_snapshot").at("cameras");
    const auto& options = meta.at("options");
    const auto option_bool = [&](const char* key) { return options.at(key).get<std::string>() == "true"; };
    const auto option_float = [&](const char* key) { return std::stof(options.at(key).get<std::string>()); };
    const auto sequencer = option_bool("VR_MatchGameFOVProSpiAutoCameraSequencer");
    const auto safety = option_bool("VR_MatchGameFOVProSpiCameraSafetyGuard");
    const auto outfield_rule = option_bool("VR_MatchGameFOVProSpiCameraSafetyOutfieldRule");
    const auto outfield_y = option_float("VR_MatchGameFOVProSpiCameraSafetyOutfieldYMax");
    const auto outfield_floor = option_float("VR_MatchGameFOVProSpiCameraSafetyOutfieldMinZ");
    const auto cinematic = option_bool("VR_MatchGameFOVProSpiCinematicCameraAssist");
    std::ifstream stream(root / "events.jsonl");
    require(stream.good(), "Cannot open recorded events");
    struct Shot { size_t cameras{}, accepted{}, good{}, bad{}; float min_focus{50000}, max_focus{}; };
    std::map<std::string, Shot> shots;
    std::map<std::string, size_t> reasons;
    size_t count{}, accepted{}, malformed{};
    std::string line;
    while (std::getline(stream, line)) {
        const auto e = Json::parse(line, nullptr, false);
        if (e.is_discarded()) { ++malformed; continue; }
        const auto key = std::to_string(e.at("epoch").get<uint64_t>()) + "/" + std::to_string(e.at("cut_sequence").get<uint64_t>());
        if (e.at("kind") == "marker") {
            if (e.at("marker") == 1) { ++shots[key].bad; }
            if (e.at("marker") == 2) { ++shots[key].good; }
        }
        if (e.at("kind") != "camera") { continue; }
        const auto& a = e.at("assist");
        auto& shot = shots[key]; ++shot.cameras; ++count;
        std::vector<Calibration> candidates;
        for (const auto& m : a.at("matches")) {
            const auto it = saved.find(m.at("id").get<std::string>());
            if (it == saved.end()) { candidates.clear(); break; }
            const auto& c = *it;
            candidates.push_back({{c.at("location").get<Vector>(), c.at("rotation").get<Vector>(), c.at("raw_fov")},
                m.at("focus"), m.at("min_fov"), m.at("multiplier"), m.at("confidence"), c.at("preset") == 10 || c.at("preset") == 11});
        }
        float total{}, focus{};
        for (const auto& c : candidates) { const auto w = c.confidence * c.confidence; total += w; focus += w * c.focus; }
        const auto fov = a.at("input").at("fov").get<float>();
        const auto pose = read_pose(a.at("input"));
        const auto offsets = a.at("base_offsets").get<Vector>();
        const Request r{
            .camera = pose, .mode = calibrated_mode, .prospi = true,
            .dolly_enabled = a.at("dolly_enabled"), .sequencer_enabled = sequencer,
            .safety_enabled = safety && outfield_rule && pose.location[1] <= outfield_y,
            .exact_override = a.at("calibration_applied"), .learned = a.at("source") == 3,
            .eligible_family = (a.at("preset") == 10 || a.at("preset") == 11) && a.at("dolly_source") == "OutfieldHomeFovCeiling",
            .protected_risk = cinematic || a.at("segment_latch").get<bool>(),
            .focus = total > 0 ? focus / total : a.at("focus_after").get<float>(),
            .legacy_ceiling = fov > 35 ? 4400.0f : fov > 20 ? 5500.0f : fov > 8 ? 6500.0f : 7600.0f,
            .effective_fov = a.at("effective_fov"), .base_fov = a.at("base_fov"),
            .forward_offset = offsets[0], .right_offset = offsets[1], .up_offset = offsets[2],
            .max_safety_up = a.at("safety_max_up"),
            .required_floor = std::clamp(outfield_floor, -500.0f, 3000.0f),
        };
        const auto d = evaluate(r, candidates); ++reasons[status_name(d.status)];
        if (d.accepted()) {
            ++accepted; ++shot.accepted;
            shot.min_focus = (std::min)(shot.min_focus, d.learned_focus);
            shot.max_focus = (std::max)(shot.max_focus, d.learned_focus);
        }
    }
    Json changed = Json::array();
    for (const auto& [key, s] : shots) {
        if (s.accepted) { changed.push_back({{"shot", key}, {"cameras", s.cameras}, {"accepted", s.accepted},
            {"good_marks", s.good}, {"bad_marks", s.bad}, {"candidate_focus_cm", {s.min_focus, s.max_focus}}}); }
    }
    Json report{{"schema", 1}, {"auto_apply", false}, {"camera_samples", count}, {"accepted_samples", accepted},
        {"malformed_lines", malformed}, {"reasons", reasons}, {"changed_shots", changed},
        {"limits", {"Shadow uses recorded selected matches; no runtime reclassification or new candidate search.",
            "Startup settings are from metadata; settings changes require a fresh recording.",
            "Soft-good overlapping cuts can change. No phase/ball/geometry correctness is inferred."}}};
    std::ofstream file(output);
    require(file.good(), "Cannot create shadow review"); file << report.dump(2) << '\n';
    require(file.good(), "Could not write shadow review");
    std::cout << "Shadow cameras=" << count << " accepted=" << accepted << " changed shots=" << changed.size() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--shadow") { return shadow(argv[2], argv[3]); }
        require(argc == 2, "Usage: tests fixture.json OR --shadow session-path output.json");
        const auto fixtures = read_json(argv[1]);
        captured_fixtures(fixtures); guards(fixtures);
        std::cout << "ProSpi calibrated-framing guards passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
