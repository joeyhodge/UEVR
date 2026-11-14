#include "mods/vr/ProSpiCelebrationFraming.hpp"
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace uevr::prospi::celebration;
using Vector = uevr::prospi::framing::Vector;
using Json = nlohmann::json;
namespace {
void require(bool ok, const std::string& why) {
    if (!ok) { throw std::runtime_error(why); }
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream f(path); require(f.good(), "Cannot open " + path.string()); return Json::parse(f);
}
Pose pose(const Json& j) {
    return {j.at("location").get<Vector>(), j.at("rotation").get<Vector>(), j.at("fov")};
}
Gates enabled() { return {3, true, true, true, true, true, false, false}; }
Pose rig() { return {{2137.1f, 733.9f, 112.14f}, {3.0f, 159.18f, 0}, 15.93f}; }
Calibration calibration() { return {rig(), 220, 16.5f, .999f, .996f, false}; }
Identity identity() { return {1, 2, 3, 0, 4, 5}; }

// Independently reproduce the applied stereo basis, rather than the height helper.
float stereo_z(const Pose& p, float forward, float right, float up) {
    const auto q = glm::normalize(glm::quat(glm::yawPitchRoll(glm::radians(-p.rotation[1]),
        glm::radians(p.rotation[0]), glm::radians(-p.rotation[2]))));
    const auto convert = glm::quat(glm::mat4{
        0, 0, -1, 0,
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 0, 1});
    return p.location[2] + (convert * (q * glm::vec3{0, 0, forward})).z +
        (convert * (q * glm::vec3{-right, 0, 0})).z + (convert * (q * glm::vec3{0, -up, 0})).z;
}
void policy_guards() {
    const auto p = rig(); const auto c = calibration();
    require(low_rig(p) && short_focus(p, {&c, 1}) == 220, "Close short calibration rejected");
    auto cs = std::vector<Calibration>{c, c}; cs[1].focus = 240;
    require(short_focus(p, cs) && std::abs(*short_focus(p, cs) - 230) < .01f, "Short focus blend");
    cs[1].focus = 400; require(!short_focus(p, cs), "Conflicting focus accepted");
    require(!short_focus(p, {}), "Missing match accepted");
    cs.assign(4, c); require(!short_focus(p, cs), "Too many matches accepted");
    for (auto field : {&Calibration::focus, &Calibration::min_fov, &Calibration::multiplier, &Calibration::confidence}) {
        for (const auto bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
            auto v = c; v.*field = bad; require(!short_focus(p, {&v, 1}), "Invalid saved value accepted");
        }
    }
    for (const auto n : {0, 1, 2, 4}) { auto g = enabled(); g.mode = n; require(!g.enabled(), "Historical mode affected"); }
    for (auto field : {&Gates::prospi, &Gates::dolly, &Gates::sequencer, &Gates::safety, &Gates::field_floor}) {
        auto g = enabled(); g.*field = false; require(!g.enabled(), "Capability gate bypassed");
    }
    for (auto field : {&Gates::legacy_cinematic, &Gates::decoupled_pitch}) {
        auto g = enabled(); g.*field = true; require(!g.enabled(), "Incompatible option accepted");
    }
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto v = c;
        switch (mutation) {
        case 0: v.focus = 1251; break;
        case 1: v.confidence = .827f; break;
        case 2: v.confidence = 1.01f; break;
        case 3: v.pose.location[0] += 501; break;
        case 4: v.pose.rotation[1] += 31; break;
        case 5: v.pose.location[0] = -v.pose.location[0]; v.pose.rotation[1] = -65; break;
        }
        require(!short_focus(p, {&v, 1}), "Distant, weak or opposite-side calibration accepted");
    }
    for (int mutation = 0; mutation < 9; ++mutation) {
        auto v = p;
        switch (mutation) {
        case 0: v.location[0] = 0; break;
        case 1: v.location[1] = -11000; break;
        case 2: v.location[2] = 1700; break;
        case 3: v.location[0] = 5000; break;
        case 4: v.fov = 90; break;
        case 5: v.rotation[0] = -45; break;
        case 6: v.rotation[2] = .011f; break;
        case 7: v.rotation[1] = 0; break;
        case 8: v.location[2] = std::numeric_limits<float>::quiet_NaN(); break;
        }
        require(!low_rig(v) && !short_focus(v, {&c, 1}), "Unproven rig accepted");
    }
    auto v = p; v.rotation[1] += 360;
    require(low_rig(v) && short_focus(v, {&c, 1}), "Wrapped yaw rejected");
}
void safety_guards() {
    const SafetyRequest base{enabled(), rig(), 900, 7.2774f, -5.5648f, 0, 170, 400, true};
    for (const auto pitch : {-18.0f, -10.0f, 0.0f, 10.0f, 16.0f}) {
        for (const auto roll : {-.009f, 0.0f, .009f}) {
            auto r = base; r.camera.rotation[0] = pitch; r.camera.rotation[2] = roll;
            const auto d = signed_safety(r); require(d.accepted, "Supported signed geometry rejected");
            require(d.dolly >= 0 && d.dolly <= r.dolly && d.lift >= 0 && d.lift <= r.max_lift,
                "Budget exceeded");
            const auto actual = stereo_z(r.camera, r.forward + d.dolly, r.right, r.up + d.lift);
            require(actual >= r.floor + 1.0f && std::abs(actual - d.end_z) < .15f, "Applied basis disagrees with safety");
            if (pitch >= 10) { require(d.lift == 0, "Upward dolly received spurious lift"); }
        }
    }
    auto r = base; r.camera.location[2] = 30; r.camera.rotation[0] = -18; r.dolly = 1250;
    const auto capped = signed_safety(r);
    require(capped.accepted && capped.dolly < r.dolly && std::abs(capped.lift - r.max_lift) < .02f,
        "Negative-pitch cap did not honor lift budget");
    auto reject = [&](const SafetyRequest& v) {
        const auto d = signed_safety(v); require(!d.accepted && d.dolly == 0 && d.lift == 0, "Unsafe proposal published");
    };
    r = base; r.field_zone = false; reject(r);
    r = base; r.gates.decoupled_pitch = true; reject(r);
    r = base; r.camera.rotation[2] = .011f; reject(r);
    r = base; r.dolly = -1; reject(r);
    r = base; r.dolly = 1251; reject(r);
    r = base; r.forward = 101; reject(r);
    r = base; r.right = 101; reject(r);
    r = base; r.up = -101; reject(r);
    r = base; r.max_lift = 601; reject(r);
    r = base; r.floor = 351; reject(r);
    r = base; r.max_lift = 0; r.dolly = 0; reject(r);
    for (auto field : {&SafetyRequest::dolly, &SafetyRequest::forward, &SafetyRequest::right, &SafetyRequest::up,
        &SafetyRequest::floor, &SafetyRequest::max_lift}) {
        for (const auto bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
            -std::numeric_limits<float>::infinity()}) { r = base; r.*field = bad; reject(r); }
    }
}
void continuity_guards() {
    FocusContinuity c; const auto p = rig(); const auto id = identity();
    auto step = [&](double t, float target, bool learned = false) { return c.update(true, false, false, id, p, t, target, learned); };
    require(step(0, 220, true) == 220 && step(.1, 1038) == 220 && step(.24, 1038) == 220, "Brief match loss not held");
    require(std::abs(step(.3, 1038) - 256) < .01f, "Match hold exceeded 250 ms");
    require(step(.6, 1038) == 1038, "Long gap retained target");
    for (int change = 0; change < 6; ++change) {
        c.reset(); step(0, 220, true); auto next = id;
        switch (change) {
        case 0: ++next.world; break;
        case 1: ++next.camera_manager; break;
        case 2: ++next.view_target; break;
        case 3: ++next.rendering_method; break;
        case 4: ++next.target_index; break;
        case 5: ++next.target_serial; break;
        }
        require(c.update(true, false, false, next, p, .01, 1038, false) == 1038, "Identity change did not reset");
    }
    for (int change = 0; change < 7; ++change) {
        c.reset(); step(0, 220, true); auto next = p; bool cut{}, exact{}, on{true}; auto next_id = id;
        switch (change) {
        case 0: cut = true; break;
        case 1: exact = true; break;
        case 2: on = false; break;
        case 3: next_id.view_target = 0; break;
        case 4: next.location[1] -= 800; break;
        case 5: next.rotation[1] += 26; break;
        case 6: next.fov += 11; break;
        }
        require(c.update(on, exact, cut, next_id, next, .01, 1038, false) == 1038, "Cut/capability reset failed");
    }
    c.reset(); step(1, 220, true); require(step(.5, 1038) == 1038, "Time reversal retained target");
    c.reset(); step(0, 220, true);
    require(step(.01, 3000) == 3000 && step(.02, 1038) == 1038, "Unsupported focus retained state");
    auto converge = [&](int hz) {
        c.reset(); step(0, 220);
        float value{}; for (int i = 1; i <= hz; ++i) { value = step(static_cast<double>(i) / hz, 1038); }
        return value;
    };
    require(std::abs(converge(30) - converge(120)) < .1f && std::abs(converge(30) - 820) < .1f,
        "Focus continuity depends on frame rate");
    c.reset(); step(0, 1038);
    for (int i = 1; i <= 3000; ++i) {
        require(step(i / 30.0, 1038) == 1038, "Continuous shot gained a timeout discontinuity");
    }
}

void baseline_height_guards() {
    // Quantized yaw/position families from the Oct 7 log, not an exact celebration recording.
    SafetyRequest r{enabled(), {{3500, -1500, 0}, {-5, 110, 0}, 10}, 881.2f, 0, 0, 0, 170, 400, true, true};
    require(baseline_field_floor(r, true, true, false), "Low first-base pan lost field-floor precedence");
    auto d = signed_safety(r);
    require(d.accepted && d.dolly == r.dolly && d.lift < 260 && d.end_z >= 171,
        "Ground-level first-base pan retained the stand-height jump");
    const auto first_height = d.end_z;
    for (const auto yaw : {54.0f, 55.0f, 110.0f, 124.9f, 125.0f, 160.0f, 190.0f, 360.0f}) {
        r.camera.rotation[1] = yaw;
        require(baseline_field_floor(r, true, true, false), "Yaw boundary promoted a ground rig to stands");
        d = signed_safety(r);
        require(d.accepted && d.dolly == r.dolly && std::abs(d.end_z - first_height) < .01f,
            "Signed baseline height jumped during an authored pan");
        require(std::abs(stereo_z(r.camera, d.dolly, 0, d.lift) - d.end_z) < .01f,
            "Baseline height disagrees with applied stereo basis");
    }
    r.camera.location[0] = -3500; r.camera.rotation[0] = 0; r.dolly = 992.7f;
    for (const auto yaw : {-115.0f, -100.0f, -45.1f, -45.0f, 20.0f, 75.0f}) {
        r.camera.rotation[1] = yaw;
        d = signed_safety(r);
        require(baseline_field_floor(r, true, true, false) && d.accepted &&
            d.dolly == r.dolly && std::abs(d.lift - 172) < .01f,
            "Third-base pan retained excessive stand lift");
    }
    r.camera = {{3500, -1500, 200}, {5, 0, 0}, 10};
    d = signed_safety(r);
    require(d.accepted && d.dolly == r.dolly && d.lift == 0,
        "Upward baseline dolly received a false below-field correction");
    require(!baseline_field_floor(r, false, true, false) && !baseline_field_floor(r, true, false, false) &&
        !baseline_field_floor(r, true, true, true), "Off/manual/protected floor selection changed");
    auto old = r; old.baseline_camera = false;
    require(!signed_safety(old).accepted, "Historical yaw envelope widened without the guard");
    for (auto mode : {0, 1, 2, 4}) {
        auto v = r; v.gates.mode = mode;
        require(!baseline_field_floor(v, true, true, false) && !signed_safety(v).accepted,
            "New baseline policy changed a historical mode");
    }
    for (auto field : {&Gates::prospi, &Gates::dolly, &Gates::sequencer, &Gates::safety, &Gates::field_floor}) {
        auto v = r; v.gates.*field = false;
        require(!baseline_field_floor(v, true, true, false), "Baseline floor bypassed a capability gate");
    }
    for (int change = 0; change < 13; ++change) {
        auto v = r;
        switch (change) {
        case 0: v.camera.location[2] = 1500; break;
        case 1: v.camera.location[1] = -6000; break;
        case 2: v.camera.location[0] = 6000; break;
        case 3: v.camera.fov = 24.01f; break;
        case 4: v.camera.fov = .49f; break;
        case 5: v.camera.rotation[2] = .011f; break;
        case 6: v.camera.rotation[0] = -18.01f; break;
        case 7: v.camera.location[2] = std::numeric_limits<float>::quiet_NaN(); break;
        case 8: v.forward = 1; break;
        case 9: v.right = 1; break;
        case 10: v.up = 1; break;
        case 11: v.dolly = 2001; break;
        case 12: v.floor = 1705; break;
        }
        require(!baseline_field_floor(v, true, true, false), "Unsupported rig/budget/offset replaced the stand policy");
    }
    auto high = r; high.camera.location[2] = 1500;
    require(!signed_safety(high).accepted, "Genuine stand camera treated as a ground-level rig");
}

bool option(const Json& j, const char* key) { return j.value(key, std::string{"false"}) == "true"; }
float number(const Json& j, const char* key, float fallback) { return std::stof(j.value(key, std::to_string(fallback))); }
std::vector<Calibration> matches(const Json& a, const Json& saved) {
    std::vector<Calibration> out;
    for (const auto& m : a.at("matches")) {
        const auto it = saved.find(m.at("id").get<std::string>());
        if (it == saved.end()) { return {}; }
        out.push_back({{it->at("location").get<Vector>(), it->at("rotation").get<Vector>(), it->at("raw_fov")},
            m.at("focus"), m.at("min_fov"), m.at("multiplier"), m.at("confidence"), false});
    }
    return out;
}
struct Proposal {
    float focus{}, dolly{}, lift{}, end_z{};
    bool restored{}, continuous{}, signed_height{};
};
Proposal simulate(const Json& a, const Json& o, const Json& saved, FocusContinuity& continuity, double time, int mode = 3) {
    const auto p = pose(a.at("input")); const auto offsets = a.at("base_offsets").get<Vector>();
    const Gates g{mode, true, a.at("dolly_enabled"), option(o, "VR_MatchGameFOVProSpiAutoCameraSequencer"),
        option(o, "VR_MatchGameFOVProSpiCameraSafetyGuard") && !option(o, "VR_MatchGameFOVProSpiActualClamp") &&
        !a.at("stabilizer").get<bool>() && !option(o, "VR_MatchGameFOVProSpiCameraSafetyBaselineRule") &&
        !(option(o, "VR_MatchGameFOVGenericCameraPresets") && option(o, "VR_MatchGameFOVGenericCameraPresetsAutoApply")) &&
        number(o, "VR_MatchGameFOVProSpiCameraSafetyDollyCapStrength", 1) == 1,
        option(o, "VR_MatchGameFOVProSpiCameraSafetyFieldRule") &&
        (!option(o, "VR_MatchGameFOVProSpiCameraSafetyStandRule") || std::abs(p.location[0]) <
            std::clamp(number(o, "VR_MatchGameFOVProSpiCameraSafetyStandXMin", 2500), 0.0f, 10000.0f)) &&
        (!option(o, "VR_MatchGameFOVProSpiCameraSafetyOutfieldRule") || p.location[1] >
            std::clamp(number(o, "VR_MatchGameFOVProSpiCameraSafetyOutfieldYMax", -6000), -20000.0f, 5000.0f)),
        option(o, "VR_MatchGameFOVProSpiCinematicCameraAssist"), option(o, "VR_DecoupledPitch")};
    SafetyRequest r{g, p, 0, offsets[0], offsets[1], offsets[2],
        number(o, "VR_MatchGameFOVProSpiCameraSafetyFieldMinZ", 170), a.at("safety_max_up"), true};
    Proposal result{a.at("focus_after"), a.at("dolly_after"), a.at("safety_up")};
    const auto available = signed_safety(r).accepted;
    const auto base = a.at("base_fov").get<float>(), effective = a.at("effective_fov").get<float>();
    const auto forward_zoom = std::isfinite(base) && base >= 5 && base <= 175 &&
        std::isfinite(effective) && effective >= 5 && effective < base;
    std::optional<float> learned;
    if (available && forward_zoom && !a.at("calibration_applied").get<bool>() && a.at("source") == 3) {
        learned = short_focus(p, matches(a, saved));
    }
    const Identity id{a.at("world"), a.at("pcm"), a.at("view_target"), a.at("rendering_method")};
    if (available && forward_zoom && !a.at("calibration_applied").get<bool>() && id.valid() &&
        (learned || result.focus <= 1250)) {
        if (learned) { result.focus = *learned; result.restored = true; }
        const auto next = continuity.update(true, false, a.at("cut_known").get<bool>() && a.at("cut").get<bool>(),
            id, p, time, result.focus, learned.has_value());
        result.continuous = std::abs(result.focus - next) > .01f; result.focus = next;
    } else { continuity.reset(); }
    r.dolly = result.focus * (1 - std::tan(effective * std::numbers::pi_v<float> / 360) /
        std::tan(base * std::numbers::pi_v<float> / 360));
    const auto d = signed_safety(r);
    if (d.accepted) { result.dolly = d.dolly; result.lift = d.lift; result.signed_height = true; }
    require(!(result.restored || result.continuous) || d.accepted, "Partial focus change without a safe final geometry");
    result.end_z = stereo_z(p, offsets[0] + result.dolly, offsets[1], offsets[2] + result.lift);
    return result;
}
void captured(const Json& data, const Json& center) {
    const auto& o = data.at("options"); const auto& saved = data.at("calibrations");
    FocusContinuity continuity; size_t restored{}, signed_count{};
    float old_max{}, new_max{}, largest_step{}; float previous{}; int previous_cut{-1};
    for (const auto& sample : data.at("samples")) {
        const auto& a = sample.at("assist"); const auto t = sample.at("time_ns").get<double>() / 1e9;
        const auto proposed = simulate(a, o, saved, continuity, t);
        restored += proposed.restored; signed_count += proposed.signed_height;
        require(proposed.signed_height && proposed.dolly > 0, "Recorded celebration still collapses to zero");
        const auto& view = sample.at("recorded_view"); const auto vp = pose(view.at("input"));
        const auto offsets = a.at("base_offsets").get<Vector>();
        const auto old_z = stereo_z(vp, offsets[0] + a.at("dolly_after").get<float>(), offsets[1],
            offsets[2] + a.at("safety_up").get<float>());
        require(std::abs(old_z - view.at("neutral").at("location")[2].get<float>()) < .1f,
            "Independent basis does not reproduce recorded neutral view");
        const auto actual = stereo_z(vp, offsets[0] + proposed.dolly, offsets[1], offsets[2] + proposed.lift);
        require(actual >= 170.0f, "Recorded next-view geometry falls below configured floor");
        if (sample.at("cut_sequence") == 103) {
            old_max = (std::max)(old_max, old_z); new_max = (std::max)(new_max, actual);
            if (previous_cut == 103) { largest_step = (std::max)(largest_step, std::abs(proposed.dolly - previous)); }
        }
        if (sample.at("cut_sequence") == 101 && proposed.restored) {
            require(std::abs(proposed.focus - 220) < .01f, "Same 220 cm calibration lost at old FOV floor boundary");
        }
        previous_cut = sample.at("cut_sequence"); previous = proposed.dolly;
        for (int mode = 0; mode <= 2; ++mode) {
            FocusContinuity legacy;
            const auto d = simulate(a, o, saved, legacy, t, mode);
            require(!d.signed_height && !d.restored && !d.continuous && d.focus == a.at("focus_after") &&
                d.dolly == a.at("dolly_after") && d.lift == a.at("safety_up"), "Recorded old mode altered");
        }
    }
    require(restored > 25 && signed_count == data.at("samples").size(), "Fixture coverage incomplete");
    require(old_max > 450 && new_max < 300 && largest_step < 15, "Recorded pan still jumps or over-lifts");
    const auto& first = data.at("samples").front(); const auto& first_assist = first.at("assist");
    const auto first_time = first.at("time_ns").get<double>() / 1e9;
    for (const auto& [key, value] : std::map<std::string, std::string>{
        {"VR_MatchGameFOVProSpiAutoCameraSequencer", "false"}, {"VR_MatchGameFOVProSpiCameraSafetyGuard", "false"},
        {"VR_MatchGameFOVProSpiCameraSafetyFieldRule", "false"}, {"VR_MatchGameFOVProSpiCameraSafetyBaselineRule", "true"},
        {"VR_MatchGameFOVProSpiCinematicCameraAssist", "true"}, {"VR_MatchGameFOVProSpiActualClamp", "true"},
        {"VR_DecoupledPitch", "true"}, {"VR_MatchGameFOVProSpiCameraSafetyDollyCapStrength", "0.5"},
        {"VR_MatchGameFOVProSpiCameraSafetyStandXMin", "1000"}, {"VR_MatchGameFOVProSpiCameraSafetyOutfieldYMax", "2000"},
        {"VR_MatchGameFOVProSpiCameraSafetyFieldMinZ", "1000"}}) {
        auto options = o; options[key] = value; FocusContinuity state;
        const auto d = simulate(first_assist, options, saved, state, first_time);
        require(!d.signed_height && !d.restored && !d.continuous && d.focus == first_assist.at("focus_after") &&
            d.dolly == first_assist.at("dolly_after"), "Unsupported configuration did not fall back: " + key);
    }
    auto exact = first_assist; exact["calibration_applied"] = true; FocusContinuity state;
    require(simulate(exact, o, saved, state, first_time).focus == exact.at("focus_after"), "Exact focus override altered");
    auto invalid = first_assist; invalid["view_target"] = 0;
    require(!simulate(invalid, o, saved, state, first_time).restored, "Unproven view target committed short focus");
    for (const auto& f : center.at("fixtures")) {
        const auto& request = f.at("request"); const auto p = pose(request.at("camera"));
        if (request.at("eligible_family").get<bool>()) {
            require(!low_rig(p) && !signed_safety({enabled(), p, 1000, 0, 0, 0, 170, 400, true}).accepted,
                "Center-field pitching path changed");
        }
    }
    std::cout << "Captured celebration samples=" << signed_count << " restored=" << restored <<
        " max neutral height=" << old_max << " -> " << new_max << " cm, max pan dolly step=" << largest_step << " cm\n";
}
int shadow(const std::filesystem::path& root, const std::filesystem::path& output) {
    const auto meta = read_json(root / "metadata.json"); const auto& o = meta.at("options");
    const auto& saved = meta.at("calibration_snapshot").at("cameras");
    std::ifstream input(root / "events.jsonl"); require(input.good(), "Cannot open recording");
    struct Shot { size_t cameras{}, changed{}, restored{}, height{}, good{}, bad{}; float old_max{}, new_max{}; };
    std::map<std::string, Shot> shots; FocusContinuity continuity;
    size_t count{}, changed{}, center_accepted{}, center_changed{}, malformed{}, pitch_observations{}; std::string line;
    while (std::getline(input, line)) {
        const auto e = Json::parse(line, nullptr, false);
        if (e.is_discarded()) { ++malformed; continuity.reset(); continue; }
        const auto key = std::to_string(e.at("epoch").get<uint64_t>()) + "/" + std::to_string(e.at("cut_sequence").get<uint64_t>());
        auto& shot = shots[key];
        if (e.at("kind") == "marker") { shot.bad += e.at("marker") == 1; shot.good += e.at("marker") == 2; }
        if (e.at("kind") == "view" && !o.contains("VR_DecoupledPitch")) {
            require(!e.at("view").at("decoupled_pitch").get<bool>(), "Legacy trace has unsupported decoupled-pitch observations");
            ++pitch_observations;
        }
        if (e.at("kind") != "camera") { continue; }
        const auto& a = e.at("assist"); ++count; ++shot.cameras;
        const auto d = simulate(a, o, saved, continuity, e.at("time_ns").get<double>() / 1e9);
        const auto diff = std::abs(d.focus - a.at("focus_after").get<float>()) > .01f ||
            std::abs(d.dolly - a.at("dolly_after").get<float>()) > .01f || std::abs(d.lift - a.at("safety_up").get<float>()) > .01f;
        if (a.value("framing_status", 0) == 1) { ++center_accepted; center_changed += diff; }
        if (!diff) { continue; }
        ++changed; ++shot.changed; shot.restored += d.restored; shot.height += d.signed_height;
        const auto offsets = a.at("base_offsets").get<Vector>();
        shot.old_max = (std::max)(shot.old_max, stereo_z(pose(a.at("input")), offsets[0] +
            a.at("dolly_after").get<float>(), offsets[1], offsets[2] + a.at("safety_up").get<float>()));
        shot.new_max = (std::max)(shot.new_max, d.end_z);
    }
    require(center_changed == 0, "Confirmed center-field path changed in shadow review");
    require(o.contains("VR_DecoupledPitch") || pitch_observations != 0, "Missing pitch-state evidence");
    Json changes = Json::array();
    for (const auto& [key, s] : shots) {
        if (s.changed) { changes.push_back({{"cut", key}, {"samples", s.cameras}, {"changed", s.changed},
            {"restored_focus", s.restored}, {"signed_height", s.height}, {"good_marks", s.good}, {"bad_marks", s.bad},
            {"max_neutral_z_cm", {s.old_max, s.new_max}}}); }
    }
    const Json report{{"schema", 1}, {"auto_apply", false}, {"source_session", root.filename().string()},
        {"stadium", "Tokyo Dome, confirmed separately; recorded metadata remains unchanged"},
        {"camera_samples", count}, {"changed_samples", changed}, {"center_accepted_samples", center_accepted},
        {"center_changed_samples", center_changed}, {"malformed_lines", malformed}, {"changed_cuts", changes},
        {"legacy_nondecoupled_view_observations", pitch_observations},
        {"limits", {"Uses the recorded selected matches and startup settings with candidate mode 3; no live reclassification.",
            "Live view-target liveness and serials cannot be established offline; retained numeric identities are simulated.",
            "Soft-good marks can overlap affected shots and are not proof of ideal focus, subjects or collision geometry.",
            "Heights outside the captured fixture use the recorded cache pose, not a later renderer observation."}}};
    std::ofstream out(output); require(out.good(), "Cannot create shadow report"); out << report.dump(2) << '\n';
    require(out.good(), "Cannot finish shadow report");
    std::cout << "Shadow cameras=" << count << " changed=" << changed << " cuts=" << changes.size() <<
        " confirmed center unchanged=" << center_accepted << '\n';
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string{argv[1]} == "--shadow") { return shadow(argv[2], argv[3]); }
        require(argc == 3, "Usage: tests celebration.json center.json OR --shadow session output.json");
        policy_guards(); safety_guards(); continuity_guards(); baseline_height_guards(); captured(read_json(argv[1]), read_json(argv[2]));
        std::cout << "ProSpi low-rig celebration guards passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
