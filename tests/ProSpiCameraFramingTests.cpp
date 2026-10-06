#include "mods/vr/ProSpiCameraFraming.hpp"
#include "mods/vr/ProSpiNativeFocusGuard.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
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

namespace fg = uevr::prospi::focus_guard;
namespace trace = uevr::prospi::trace;

fg::Request focus_request() {
    return {.camera = {{350, -3300, 50}, {1.87921035f, 47.73176193f, 0}, 3.07712674f, 0, 0, true},
        .camera_manager = 0x100000, .now_ns = 1'000'000'000, .mode = 3, .zone = 1,
        .enabled = true, .prospi = true, .dolly_enabled = true, .sequencer_enabled = true,
        .safety_enabled = true, .automatic_source = true, .dolly = 2800,
        .lift = 211.617249f, .predicted_z = -41.617249f, .floor = 170, .max_lift = 400, .base_fov = 90};
}
trace::NativeSource focus_source(const fg::Request& r, float focus = 2789.7473f) {
    trace::NativeSource s{};
    s.pose = r.camera; s.pose.aspect = 16.0f / 9.0f; s.pose.aspect_valid = true;
    s.camera_manager = r.camera_manager; s.object = 0x200000; s.camera_actor = 0x300000;
    s.time_ns = r.now_ns; s.focus_distance = focus; s.status = trace::SourceStatus::accepted;
    const auto forward = trace::forward(s.pose);
    for (size_t i = 0; i < 3; ++i) { s.look_at[i] = s.pose.location[i] + forward[i] * focus; }
    return s;
}
trace::Pose focus_endpoint(const fg::Request& r, const fg::Decision& d, float aspect) {
    auto p = r.camera; p.fov = r.base_fov; p.aspect = aspect; p.aspect_valid = true;
    constexpr float radians = 0.017453292519943295f;
    const auto pitch = p.rotation[0] * radians, yaw = p.rotation[1] * radians, roll = p.rotation[2] * radians;
    const auto forward = trace::forward(p);
    const trace::Vector right0{-std::sin(yaw), std::cos(yaw), 0};
    const trace::Vector up0{-std::sin(pitch) * std::cos(yaw), -std::sin(pitch) * std::sin(yaw), std::cos(pitch)};
    for (size_t i = 0; i < 3; ++i) {
        p.location[i] += forward[i] * d.dolly + (std::cos(roll) * up0[i] + std::sin(roll) * right0[i]) * d.lift;
    }
    return p;
}
void check_focus_endpoint(const fg::Request& r, const trace::NativeSource& s, const fg::Decision& d) {
    const auto p = focus_endpoint(r, d, s.pose.aspect);
    const auto f = trace::target_framing(p, s.look_at);
    require(d.dolly <= r.dolly && d.dolly >= 0 && d.lift <= r.lift && d.lift <= r.max_lift,
        "Focus guard increased travel/lift or exceeded budget");
    require(p.location[2] >= r.floor + 1.999f && std::abs(p.location[2] - d.end_z) < .01f,
        "Focus guard dropped the configured floor");
    require(f.valid && !f.behind && f.clip_estimate_valid && std::abs(f.ndc_x) <= .951f && std::abs(f.ndc_y) <= .951f,
        "Focus guard endpoint lost authored point");
    require(std::abs(f.depth - d.target_depth) < .01f, "Reported focus depth disagrees with geometry");
}
void native_focus_guards() {
    const auto base = focus_request();
    const auto source = focus_source(base);
    const auto decision = fg::evaluate(base, source);
    require(fg::eligible(base), "Valid recorded rig is ineligible");
    require(decision.status == fg::Status::target_capped && decision.dolly < 2789.7473f && decision.lift < 40,
        "Captured near-home overshoot not corrected");
    check_focus_endpoint(base, source, decision);
    auto expect_fallback = [&](fg::Request r, trace::NativeSource s, const char* name) {
        const auto d = fg::evaluate(r, s);
        require(!d.applied() && d.dolly == r.dolly && d.lift == r.lift && d.predicted_z == r.predicted_z, name);
    };
    for (auto field : {&fg::Request::enabled, &fg::Request::prospi, &fg::Request::dolly_enabled,
                      &fg::Request::sequencer_enabled, &fg::Request::safety_enabled, &fg::Request::automatic_source}) {
        auto r = base; r.*field = false; expect_fallback(r, source, "Disabled capability changed legacy offsets");
    }
    auto r = base; auto s = source;
    r.protected_camera = true; expect_fallback(r, source, "Explicit/protected camera changed");
    for (int mode : {0, 1, 2, 4}) { r = base; r.mode = mode; expect_fallback(r, source, "Other mode changed"); }
    for (int zone : {0, 2, 3}) { r = base; r.zone = zone; expect_fallback(r, source, "Stand/dugout/unsupported zone changed"); }
    r = base; r.camera.location[2] = 538; expect_fallback(r, focus_source(r), "Good behind-pitcher camera changed");
    r = base; r.camera = {{200, 2350, 800}, {-17.5703f, -109.5995f, 0}, 18.2955f, 0, 0, true};
    r.dolly = 2214.476f; r.lift = 0; expect_fallback(r, focus_source(r, 2253.628f), "Good high negative-pitch cut changed");
    r = base; r.camera.rotation[0] = -1; expect_fallback(r, focus_source(r), "Negative pitch changed");
    r = base; r.camera.location = {3650, -1900, 30};
    expect_fallback(r, focus_source(r), "First-base celebration-like rig changed");
    r = base; r.camera.rotation[2] = 1; expect_fallback(r, focus_source(r), "Rolled camera accepted");
    r = base; r.floor = 350; r.max_lift = r.lift = 1;
    expect_fallback(r, focus_source(r), "Insufficient lift budget accepted");
    for (auto field : {&fg::Request::forward_offset, &fg::Request::right_offset, &fg::Request::up_offset}) {
        r = base; r.*field = 1; expect_fallback(r, source, "Manual camera offset changed");
        require(!fg::eligible(r), "Unsupported manual offset would enter the native reader");
    }
    for (const float x : {-4000.0f, 4000.0f}) {
        r = base; r.zone = 4; r.camera.location = {x, -6000, 50};
        r.camera.rotation = {.69394f, x < 0 ? 56.1f : 124.0f, 0}; r.camera.fov = 3;
        r.dolly = 6500; r.lift = 203.831f; r.floor = 175; r.predicted_z = -28.831f;
        s = focus_source(r, 7214.891f);
        const auto outfield = fg::evaluate(r, s);
        require(outfield.status == fg::Status::signed_height && outfield.dolly == 6500 && outfield.lift < 70,
            "Recorded low-outfield family changed focus or retained excess lift");
        check_focus_endpoint(r, s, outfield);
        r.camera.rotation[1] = -90;
        expect_fallback(r, focus_source(r, 7214.891f), "Other outfield-facing camera accepted");
    }
    r = base;
    s = source; s.status = trace::SourceStatus::unsupported_layout;
    expect_fallback(r, s, "Unsupported native build changed offsets");
    s = source; s.status = trace::SourceStatus::changed_during_read;
    expect_fallback(r, s, "Torn native source accepted");
    s = source; s.time_ns = r.now_ns - 50'000'001; expect_fallback(r, s, "Stale source accepted");
    s = source; s.time_ns = r.now_ns + 1; expect_fallback(r, s, "Future source accepted");
    s = source; ++s.camera_manager; expect_fallback(r, s, "Old world/manager source accepted");
    s = source; s.pose.location[0] += 2; expect_fallback(r, s, "Cut-mismatched source accepted");
    s = source; s.pose.rotation[1] += .2f; expect_fallback(r, s, "Aim-mismatched source accepted");
    s = source; s.look_at[0] += 20; expect_fallback(r, s, "Off-axis/stale target accepted");
    s = source; s.focus_distance += 10; expect_fallback(r, s, "Focus/point disagreement accepted");
    s = source; s.pose.aspect_valid = false; expect_fallback(r, s, "Unavailable aspect accepted");
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()}) {
        for (auto field : {&fg::Request::floor, &fg::Request::max_lift, &fg::Request::base_fov,
                          &fg::Request::forward_offset, &fg::Request::right_offset, &fg::Request::up_offset}) {
            r = base; r.*field = bad; expect_fallback(r, source, "Non-finite guard input accepted");
        }
        s = source; s.focus_distance = bad; expect_fallback(base, s, "Non-finite native focus accepted");
        s = source; s.pose.aspect = bad; expect_fallback(base, s, "Non-finite aspect accepted");
        s = source; s.look_at[2] = bad; expect_fallback(base, s, "Non-finite target accepted");
        r = base; r.dolly = bad; require(!fg::evaluate(r, source).applied(), "Non-finite travel accepted");
        r = base; r.lift = bad; require(!fg::evaluate(r, source).applied(), "Non-finite lift accepted");
        r = base; r.predicted_z = bad; require(!fg::evaluate(r, source).applied(), "Non-finite estimate accepted");
        r = base; r.camera.location[0] = bad; require(!fg::evaluate(r, source).applied(), "Non-finite camera accepted");
    }
    // Rejection after success never carries forward a previous target or correction.
    require(fg::evaluate(base, source).applied(), "Valid source failed after rejection");
    expect_fallback(base, {}, "Previous target reused after source rejection");
    r = base; r.enabled = false; expect_fallback(r, source, "Disable retained a previous correction");
    r = base; r.camera.rotation[1] += 360; const auto wrapped = fg::evaluate(r, source);
    require(wrapped.applied() && std::abs(wrapped.dolly - decision.dolly) < .01f, "Wrapped yaw changed geometry");

    std::mt19937 rng{44150};
    const auto uniform = [&](float lo, float hi) { return std::uniform_real_distribution<float>{lo, hi}(rng); };
    size_t applied{}, height_only{}, capped{};
    for (int i = 0; i < 10000; ++i) {
        r = base; r.camera.location = {350, -3300, uniform(-100, 150)};
        r.camera.rotation = {uniform(.25f, 8), uniform(-180, 180), uniform(-.009f, .009f)};
        r.camera.fov = uniform(.1f, 8); r.dolly = uniform(0, 8000); r.floor = uniform(-50, 350); r.max_lift = 600;
        r.predicted_z = r.camera.location[2] - r.dolly * std::sin(r.camera.rotation[0] * .017453292519943295f);
        r.lift = std::clamp(r.floor - r.predicted_z, .01f, 600.0f);
        s = focus_source(r, uniform(500, 20000)); s.pose.aspect = uniform(.5f, 3);
        const auto d = fg::evaluate(r, s);
        if (d.applied()) {
            ++applied; height_only += d.status == fg::Status::signed_height; capped += d.status == fg::Status::target_capped;
            check_focus_endpoint(r, s, d);
            auto perturbed = r; perturbed.camera.location[2] -= 1;
            perturbed.camera.rotation[0] -= .1f;
            perturbed.camera.rotation[2] += std::copysign(.01f, r.camera.rotation[2]);
            require(focus_endpoint(perturbed, d, s.pose.aspect).location[2] >= r.floor + 1.998f,
                "Accepted source-pose tolerance lost floor clearance");
        }
        r.enabled = false; expect_fallback(r, s, "Randomized default-off behavior changed");
    }
    require(applied > 1000 && height_only > 100 && capped > 100, "Insufficient geometry coverage");
    std::cout << "Native focus guards: randomized applied=" << applied << " height-only=" << height_only << " capped=" << capped << '\n';
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

trace::Pose read_trace_pose(const Json& p) {
    trace::Pose out{};
    out.location = p.at("location").get<trace::Vector>(); out.rotation = p.at("rotation").get<trace::Vector>();
    out.fov = p.at("fov"); out.valid = p.at("valid");
    if (!p.at("aspect").is_null()) { out.aspect = p.at("aspect"); out.aspect_valid = true; }
    return out;
}
trace::NativeSource read_native_source(const Json& value) {
    trace::NativeSource out{};
    if (value.at("status") != "accepted" || !value.at("valid").get<bool>()) { return out; }
    out.pose = read_trace_pose(value.at("pose")); out.look_at = value.at("look_at").get<trace::Vector>();
    out.camera_manager = value.at("pcm"); out.camera_actor = value.at("camera_actor"); out.object = value.at("object");
    out.time_ns = value.at("time_ns"); out.frame = value.at("native_frame"); out.focus_distance = value.at("focus_cm");
    out.status = trace::SourceStatus::accepted;
    return out;
}
int native_shadow(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto meta = read_json(root / "metadata.json");
    require(meta.at("variant").get<std::string>().find("eBaseball") != std::string::npos, "Replay requires ProSpi trace");
    const auto& options = meta.at("options");
    const auto option = [&](const char* name) { return options.at(name).get<std::string>() == "true"; };
    const bool protected_settings = option("VR_MatchGameFOVProSpiActualClamp") || option("VR_MatchGameFOVCameraCutStabilizer") ||
        option("VR_MatchGameFOVGenericCameraPresetsAutoApply") || option("VR_MatchGameFOVProSpiCinematicCameraAssist") ||
        option("VR_DecoupledPitch") || std::stof(options.at("VR_MatchGameFOVProSpiCameraSafetyDollyCapStrength").get<std::string>()) != 1;
    struct Shot {
        size_t matched{}, applied{}, capped{}, height{}, before_behind{}, before_offscreen{}, after_behind{}, after_offscreen{};
        float min_dolly{50000}, max_dolly{}, min_lift{50000}, max_lift{}, min_clearance{50000};
        Json examples = Json::array();
    };
    std::map<std::string, Shot> shots;
    std::map<std::string, size_t> reasons;
    size_t malformed{}, cameras{}, matched{}, applied{}, tolerance_mismatch{};
    std::ifstream stream(root / "events.jsonl"); require(stream.good(), "Cannot open recorded events");
    std::string line;
    while (std::getline(stream, line)) {
        const auto e = Json::parse(line, nullptr, false);
        if (e.is_discarded()) { ++malformed; continue; }
        if (e.at("kind") != "camera" && e.at("kind") != "view") { continue; }
        const auto& a = e.at("assist");
        const auto offsets = a.at("base_offsets").get<trace::Vector>();
        const auto source = read_native_source(a.at("native_source"));
        const fg::Request r{
            .camera = read_trace_pose(a.at("input")), .camera_manager = a.at("pcm"), .now_ns = e.at("time_ns"),
            .mode = a.at("mode"), .zone = a.at("zone"), .enabled = true, .prospi = true,
            .dolly_enabled = a.at("dolly_enabled"),
            .sequencer_enabled = option("VR_MatchGameFOVProSpiAutoCameraSequencer") && a.at("sequencer_active").get<bool>(),
            .safety_enabled = option("VR_MatchGameFOVProSpiCameraSafetyGuard"),
            .protected_camera = protected_settings || a.at("calibration_applied").get<bool>() || a.at("segment_latch").get<bool>() ||
                a.at("framing_status") == 1 || a.at("framing_status") == 9 || a.at("stabilizer").get<bool>(),
            .automatic_source = a.at("source") == 2 || a.at("source") == 3,
            .dolly = a.at("dolly_after"), .lift = a.at("safety_up"), .predicted_z = a.at("safety_predicted_z"),
            .floor = a.at("safety_min_z"), .max_lift = a.at("safety_max_up"), .base_fov = a.at("base_fov"),
            .forward_offset = offsets[0], .right_offset = offsets[1], .up_offset = offsets[2],
        };
        auto disabled = r; disabled.enabled = false;
        const auto old = fg::evaluate(disabled, source);
        require(!old.applied() && old.dolly == r.dolly && old.lift == r.lift && old.predicted_z == r.predicted_z,
            "Default-off replay changed recorded camera offsets");
        if (e.at("kind") == "camera") { ++cameras; continue; }
        const auto& v = e.at("view");
        if (!v.at("source_matches_input").get<bool>() || !v.at("input_matches_assist").get<bool>() ||
            !v.at("neutral_valid").get<bool>() || v.at("decoupled_pitch").get<bool>()) { continue; }
        const auto key = std::to_string(e.at("epoch").get<uint64_t>()) + "/" + std::to_string(e.at("cut_sequence").get<uint64_t>());
        auto& shot = shots[key]; ++matched; ++shot.matched;
        const auto d = fg::evaluate(r, source); ++reasons[fg::status_name(d.status)];
        if (!d.applied()) { continue; }
        auto actual = r; actual.camera = read_trace_pose(v.at("input"));
        if (trace::distance(actual.camera.location, r.camera.location) > 1 ||
            trace::angle_delta(actual.camera.rotation[0], r.camera.rotation[0]) > .1f ||
            trace::angle_delta(actual.camera.rotation[2], r.camera.rotation[2]) > .01f) { ++tolerance_mismatch; }
        const auto before_pose = read_trace_pose(v.at("neutral"));
        const auto before = trace::target_framing(before_pose, source.look_at);
        const auto after_pose = focus_endpoint(actual, d, source.pose.aspect);
        const auto after = trace::target_framing(after_pose, source.look_at);
        ++applied; ++shot.applied;
        shot.capped += d.status == fg::Status::target_capped; shot.height += d.status == fg::Status::signed_height;
        shot.before_behind += before.behind; shot.before_offscreen += before.offscreen_estimate;
        shot.after_behind += after.behind; shot.after_offscreen += after.offscreen_estimate;
        shot.min_dolly = (std::min)(shot.min_dolly, d.dolly); shot.max_dolly = (std::max)(shot.max_dolly, d.dolly);
        shot.min_lift = (std::min)(shot.min_lift, d.lift); shot.max_lift = (std::max)(shot.max_lift, d.lift);
        shot.min_clearance = (std::min)(shot.min_clearance, after_pose.location[2] - r.floor);
        require(d.dolly <= r.dolly && d.lift <= r.lift && d.lift <= r.max_lift &&
            after_pose.location[2] >= r.floor + 1.95f && after.valid && !after.behind && !after.offscreen_estimate,
            "Replay violated floor/target/travel constraints at sequence " + std::to_string(e.at("sequence").get<uint64_t>()));
        if (shot.examples.size() < 2 || ((before.behind || before.offscreen_estimate) && shot.examples.size() == 2)) {
            shot.examples.push_back({{"sequence", e.at("sequence")}, {"camera_id", a.at("id")},
                {"dolly_cm", {r.dolly, d.dolly}}, {"lift_cm", {r.lift, d.lift}}, {"floor_cm", r.floor},
                {"neutral_z_cm", {before_pose.location[2], after_pose.location[2]}},
                {"target_depth_cm", {before.depth, after.depth}}, {"neutral_ndc", {after.ndc_x, after.ndc_y}}});
        }
    }
    Json changed = Json::array();
    for (const auto& [key, s] : shots) {
        if (!s.applied) { continue; }
        changed.push_back({{"shot", key}, {"matched_views", s.matched}, {"applied", s.applied},
            {"capped", s.capped}, {"signed_height_only", s.height}, {"before_behind", s.before_behind},
            {"before_offscreen", s.before_offscreen}, {"after_behind", s.after_behind}, {"after_offscreen", s.after_offscreen},
            {"dolly_range_cm", {s.min_dolly, s.max_dolly}}, {"lift_range_cm", {s.min_lift, s.max_lift}},
            {"min_floor_clearance_cm", s.min_clearance}, {"examples", s.examples}});
    }
    Json report{{"schema", 1}, {"auto_apply", false}, {"camera_samples", cameras}, {"matched_views", matched},
        {"applied_views", applied}, {"malformed_lines", malformed}, {"tolerance_mismatch", tolerance_mismatch},
        {"default_off_unchanged", true}, {"statuses", reasons}, {"changed_shots", changed},
        {"limits", {"Replays the same C++ focus guard; preserves original game aim/FOV and configured floors.",
            "Frustum is a neutral-camera estimate, not asymmetric HMD or collision proof.",
            "No celebration was recorded. Other camera families and existing celebration rules stay unchanged.",
            "Runtime-only setting/override gates still need A/B validation; no profile or calibration is modified."}}};
    std::ofstream file(output); require(file.good(), "Cannot create native-focus replay report");
    file << report.dump(2) << '\n'; require(file.good(), "Could not write replay report");
    require(malformed == 0 && tolerance_mismatch == 0, "Invalid evidence or pose-tolerance mismatches in replay");
    std::cout << "Native-focus replay cameras=" << cameras << " matched views=" << matched << " applied=" << applied
        << " changed shots=" << changed.size() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--shadow") { return shadow(argv[2], argv[3]); }
        if (argc == 4 && std::string(argv[1]) == "--native-shadow") { return native_shadow(argv[2], argv[3]); }
        require(argc == 2, "Usage: tests fixture.json OR --shadow/--native-shadow session-path output.json");
        const auto fixtures = read_json(argv[1]);
        captured_fixtures(fixtures); guards(fixtures); native_focus_guards();
        std::cout << "ProSpi calibrated-framing guards passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
