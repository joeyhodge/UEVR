#include "ProSpiCameraTrace.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace uevr::prospi::trace {
namespace {
using Json = nlohmann::json;
constexpr uint64_t second = 1'000'000'000;
thread_local Event last_view{};
thread_local const void* last_view_owner{};

Json pose_json(const Pose& p) {
    return {{"location", p.location}, {"rotation", p.rotation}, {"fov", p.fov},
        {"aspect", p.aspect_valid ? Json(p.aspect) : Json(nullptr)},
        {"cache_timestamp", p.timestamp_valid ? Json(p.cache_timestamp) : Json(nullptr)},
        {"valid", valid_pose(p)}};
}
Json source_json(const NativeSource& s) {
    static constexpr const char* statuses[] = {"unavailable", "unsupported_layout", "unreadable",
        "unsupported_node", "corrupt_list", "changed_during_read", "invalid_values",
        "bridge_mismatch", "manager_mismatch", "accepted"};
    return {{"status", statuses[(size_t)s.status]}, {"valid", valid_source(s)}, {"pose", pose_json(s.pose)},
        {"look_at", valid_source(s) ? Json(s.look_at) : Json(nullptr)},
        {"focus_cm", valid_source(s) ? Json(s.focus_distance) : Json(nullptr)},
        {"object", s.object}, {"pcm", s.camera_manager}, {"camera_actor", s.camera_actor},
        {"time_ns", s.time_ns}, {"native_frame", s.frame}, {"list_nodes", s.list_nodes},
        {"capture_us", s.capture_us}, {"bridge_error_cm", s.position_error},
        {"bridge_direction_error_degrees", s.direction_error}, {"bridge_fov_error_degrees", s.fov_error},
        {"provenance", "native publisher look-at before normalization; validated against UE bridge"},
        {"subject_identity_verified", false}, {"layout", "prospi-native-camera-v1"}};
}
Json framing_json(const TargetFraming& f) {
    return {{"valid", f.valid}, {"distance_cm", f.distance}, {"depth_cm", f.depth},
        {"yaw_error_degrees", f.yaw_error}, {"pitch_error_degrees", f.pitch_error},
        {"behind", f.behind}, {"clip_estimate_valid", f.clip_estimate_valid},
        {"ndc_estimate", {f.ndc_x, f.ndc_y}}, {"offscreen_estimate", f.offscreen_estimate}};
}
Json camera_json(const Camera& c) {
    Json matches = Json::array();
    for (size_t i = 0; i < (std::min<size_t>)(c.match_count, c.matches.size()); ++i) {
        const auto& m = c.matches[i];
        matches.push_back({{"id", m.id.data()}, {"score", m.score}, {"confidence", m.confidence},
            {"weight", m.weight}, {"focus", m.focus_distance}, {"min_fov", m.min_fov}, {"multiplier", m.multiplier}});
    }
    return {{"input", pose_json(c.input)}, {"native_source", source_json(c.native_source)},
        {"world", c.world}, {"pcm", c.camera_manager},
        {"view_target", c.view_target}, {"id", c.id.data()}, {"dolly_source", c.dolly_source.data()},
        {"matches", matches}, {"preset", c.preset}, {"mode", c.mode}, {"play_mode", c.play_mode}, {"source", c.source},
        {"zone", c.zone}, {"rendering_method", c.rendering_method}, {"confidence", c.confidence},
        {"framing_status", c.framing_status}, {"framing_candidate_focus", c.framing_candidate_focus},
        {"framing_candidate_dolly", c.framing_candidate_dolly},
        {"native_focus_guard", {{"enabled", c.native_focus_guard_enabled}, {"applied", c.native_focus_guard_applied},
            {"status", c.native_focus_guard_status}, {"dolly_before", c.native_focus_guard_dolly_before},
            {"lift_before", c.native_focus_guard_lift_before}, {"end_z", c.native_focus_guard_end_z},
            {"target_depth", c.native_focus_guard_depth}, {"floor", c.native_focus_guard_floor},
            {"zone", c.native_focus_guard_zone}, {"protected", c.native_focus_guard_protected}}},
        {"base_fov", c.base_fov}, {"effective_fov", c.effective_fov}, {"min_fov", c.min_fov},
        {"multiplier", c.multiplier}, {"focus_before", c.focus_before}, {"focus_after", c.focus_after},
        {"dolly_before", c.dolly_before}, {"dolly_after", c.dolly_after},
        {"safety_min_z", c.safety_min_z}, {"safety_predicted_z", c.safety_predicted_z},
        {"safety_up", c.safety_up}, {"safety_max_up", c.safety_max_up},
        {"base_offsets", {c.camera_forward, c.camera_right, c.camera_up}}, {"smoothing", c.smoothing},
        {"calibration_applied", c.calibration_applied}, {"wrote_fov", c.wrote_fov},
        {"previous_update_wrote_fov", c.previous_update_wrote_fov}, {"cut_known", c.cut_known},
        {"cut", c.cut}, {"safety_active", c.safety_active}, {"dolly_enabled", c.dolly_enabled},
        {"stabilizer", c.stabilizer}, {"segment_latch", c.segment_latch},
        {"sequencer_active", c.sequencer_active}, {"provenance", (int)c.provenance}};
}
Json surface_json(const Surface& s) {
    return {{"query_valid", s.query_valid}, {"hit", s.hit}, {"static_component", s.static_component},
        {"point", s.point}, {"normal", s.normal}, {"distance", s.distance}, {"component", s.component}};
}
Json event_json(const Event& e) {
    static constexpr const char* kinds[] = {"camera", "view", "projection", "observation", "marker", "gap"};
    Json out{{"sequence", e.sequence}, {"time_ns", e.time_ns}, {"session", e.session}, {"epoch", e.epoch},
        {"camera_sequence", e.camera_sequence}, {"cut_sequence", e.cut_sequence}, {"thread", e.thread},
        {"kind", kinds[(int)e.kind]}, {"marker", (int)e.marker}, {"suspects", e.suspects},
        {"label", e.label.data()}, {"assist_update_us", e.capture_us}, {"inferred_cut", e.inferred_cut}};
    if (e.kind == Kind::camera || e.kind == Kind::view || e.kind == Kind::projection || e.kind == Kind::observation) {
        out["assist"] = camera_json(e.camera);
    }
    if (e.kind == Kind::view) {
        const auto& v = e.view;
        out["view"] = {{"input", pose_json(v.input)}, {"neutral", pose_json(v.neutral)},
            {"output", pose_json(v.output)}, {"forward_offset", v.forward_offset},
            {"right_offset", v.right_offset}, {"up_offset", v.up_offset},
            {"world_to_meters", v.world_to_meters}, {"world_scale", v.world_scale},
            {"snapshot_age_ms", v.snapshot_age_ms}, {"index", v.index}, {"eye", v.eye},
            {"neutral_valid", v.neutral_valid}, {"decoupled_pitch", v.decoupled_pitch},
            {"input_matches_assist", v.input_matches_assist}, {"family_role", "unclassified"},
            {"source_matches_input", v.source_matches_input},
            {"assist_offset_status", v.assist_offset_status},
            {"hmd", {{"valid", v.hmd_pose_valid}, {"rotation_xyzw", v.hmd_rotation},
                {"eye_rotation_xyzw", v.eye_rotation}, {"recenter_rotation_xyzw", v.recenter_rotation},
                {"standing_delta_runtime_units", v.standing_delta}, {"head_translation_cm", v.head_translation},
                {"eye_translation_cm", v.eye_translation}, {"head_translation_applied", v.head_translation_applied},
                {"rotation_applied", v.hmd_rotation_applied}, {"mono", v.mono}}}};
        if (v.source_matches_input && valid_source(e.camera.native_source)) {
            const auto& s = e.camera.native_source;
            out["view"]["target_framing"] = {{"source", framing_json(target_framing(s.pose, s.look_at))},
                {"neutral", framing_json(v.neutral_valid ? target_framing(v.neutral, s.look_at) : TargetFraming{})},
                {"hmd", framing_json(target_framing(v.output, s.look_at))},
                {"note", "Angular framing is measured; clip bounds are neutral-camera estimates, not asymmetric HMD visibility."}};
        }
    } else if (e.kind == Kind::projection) {
        out["projection"] = e.projection;
        out["eye"] = e.view.eye;
    } else if (e.kind == Kind::observation) {
        const auto& o = e.observation;
        out["observation"] = {{"cache", pose_json(o.cache)}, {"source_camera", pose_json(o.source_camera)},
            {"native_source", source_json(o.native_source)},
            {"ball", o.ball_valid ? Json(o.ball) : Json(nullptr)},
            {"ball_velocity", o.velocity_valid ? Json(o.ball_velocity) : Json(nullptr)},
            {"subject", o.subject_valid ? Json(o.subject) : Json(nullptr)},
            {"ball_object", o.ball_object}, {"subject_object", o.subject_object},
            {"floor", surface_json(o.floor)}, {"dolly_path", surface_json(o.dolly_path)},
            {"reference_camera", o.reference_camera}, {"stadium", o.stadium.data()},
            {"replay", o.replay_known ? Json(o.replay) : Json(nullptr)},
            {"source_camera_valid", o.source_camera_valid}, {"geometry_available", o.geometry_available},
            {"cut_known", o.cut_known}, {"cut", o.cut}};
    }
    return out;
}
}

struct Recorder::Impl {
    explicit Impl(Limits l) : limits(l) {
        limits.queue_capacity = std::clamp<size_t>(limits.queue_capacity, 1, 2048);
        limits.history_capacity = std::clamp<size_t>(limits.history_capacity, 1, 4096);
    }
    Limits limits;
    std::atomic<bool> enabled{}, stop_requested{}, done{true};
    std::atomic<Status> state{Status::off};
    std::atomic<uint64_t> written{}, dropped{}, suspects{}, bytes{}, probe_failures{};
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<Event> queue;
    size_t head{}, count{};
    uint64_t session{}, epoch{}, sequence{}, cut_sequence{}, last_camera_queued{};
    std::array<uint64_t, 4> last_view_time{}, last_view_camera{};
    std::array<uint64_t, 2> last_projection_time{}, last_projection_camera{};
    uintptr_t world{};
    Event camera{}, neutral{};
    bool have_camera{}, have_neutral{}, previous_cut{};
    std::thread worker;

    // Caller holds mutex; producers only ever take it with try_lock.
    bool push(Event e) noexcept {
        if (!enabled.load(std::memory_order_relaxed)) { return false; }
        if (count == queue.size()) { dropped.fetch_add(1, std::memory_order_relaxed); return false; }
        e.sequence = ++sequence;
        if (e.time_ns == 0) { e.time_ns = Recorder::clock_ns(); }
        e.session = session;
        e.epoch = epoch;
        if (e.camera_sequence == 0) { e.cut_sequence = cut_sequence; }
        queue[(head + count) % queue.size()] = e;
        ++count;
        wake.notify_one();
        return true;
    }
    void run(std::filesystem::path root, std::string metadata) noexcept {
        try {
            const auto directory = root / ("session-" + std::to_string(session));
            std::filesystem::create_directories(directory);
            auto meta = Json::parse(metadata);
            meta["schema"] = 2;
            meta["session"] = session;
            meta["units"] = {{"position", "Unreal cm"}, {"rotation", "pitch/yaw/roll degrees"},
                {"projection", "column-major 4x4"}, {"clock", "steady nanoseconds; session-local"}};
            meta["limits"] = {{"bytes", limits.byte_limit}, {"queue", limits.queue_capacity},
                {"history", limits.history_capacity}, {"normal_hz", 30}, {"burst_hz", 120}, {"context_seconds", 5}};
            meta["capabilities"] = {{"camera", true}, {"neutral_and_hmd_pose", true},
                {"native_look_at", "Conditional on validated code/layout, manager and per-sample bridge agreement"},
                {"hmd_offset_provenance", true},
                {"authoritative_ball", false}, {"static_geometry", false},
                {"note", "A native look-at point is not player/ball identity. Unsupported or torn samples are unavailable, never carried forward."}};
            if (meta.contains("executable") && meta["executable"].is_string()) {
                const auto executable = std::filesystem::u8path(meta["executable"].get<std::string>());
                std::error_code error;
                const auto size = std::filesystem::file_size(executable, error);
                if (!error) { meta["executable_size"] = size; }
                const auto modified = std::filesystem::last_write_time(executable, error);
                if (!error) { meta["executable_file_clock_ns"] = modified.time_since_epoch().count(); }
            }
            // Snapshot, never edit the live calibration database.
            const auto calibration = root.parent_path() / "camera_calibration.json";
            std::error_code ec;
            const auto calibration_size = std::filesystem::file_size(calibration, ec);
            if (!ec && calibration_size <= 1024 * 1024) {
                try {
                    std::ifstream in(calibration);
                    Json snapshot;
                    in >> snapshot;
                    meta["calibration_snapshot"] = std::move(snapshot);
                } catch (...) { meta["calibration_snapshot_error"] = "Unreadable or invalid JSON; live file left unchanged"; }
            }
            const auto metadata_text = meta.dump(2);
            if (metadata_text.size() >= limits.byte_limit) { throw std::runtime_error("metadata exceeds storage limit"); }
            std::ofstream metadata_file(directory / "metadata.json", std::ios::binary);
            metadata_file << metadata_text;
            metadata_file.close();
            if (!metadata_file) { throw std::runtime_error("metadata write failed"); }
            bytes.store(metadata_text.size(), std::memory_order_relaxed);
            std::ofstream stream(directory / "events.jsonl", std::ios::binary);
            if (!stream) { throw std::runtime_error("event file open failed"); }
            state.store(stop_requested.load() ? Status::stopping : Status::recording);
            struct Saved { Event event; bool saved{}; };
            std::deque<Saved> history;
            std::array<uint64_t, 18> last_saved{};
            uint64_t burst_until{}, last_flush{}, reported_drops{};
            std::array<uint32_t, 18> previous_suspects{};
            const auto save = [&](Saved& entry) {
                if (entry.saved) { return true; }
                const auto line = event_json(entry.event).dump() + '\n';
                if (bytes.load() + line.size() > limits.byte_limit) {
                    enabled.store(false);
                    stop_requested.store(true);
                    state.store(Status::storage_limit);
                    return false;
                }
                stream << line;
                if (!stream) { throw std::runtime_error("event write failed"); }
                bytes.fetch_add(line.size(), std::memory_order_relaxed);
                written.fetch_add(1, std::memory_order_relaxed);
                entry.saved = true;
                return true;
            };
            bool storage_full{};
            for (;;) {
                std::array<Event, 32> batch;
                size_t n{};
                {
                    std::unique_lock lock{mutex};
                    wake.wait_for(lock, std::chrono::milliseconds{100}, [&] { return count || stop_requested.load(); });
                    while (n < batch.size() && count) {
                        batch[n++] = queue[head];
                        head = (head + 1) % queue.size();
                        --count;
                    }
                    if (n == 0 && stop_requested.load()) { break; }
                }
                for (size_t i = 0; i < n && !storage_full; ++i) {
                    const auto& e = batch[i];
                    while (!history.empty() && (history.size() >= limits.history_capacity ||
                        (e.time_ns > history.front().event.time_ns && e.time_ns - history.front().event.time_ns > 5 * second))) {
                        history.pop_front();
                    }
                    history.push_back({e});
                    const auto key = (int)e.kind * 3 + std::clamp(e.view.eye + 1, 0, 2);
                    const auto changed_suspects = e.suspects != 0 && (e.suspects & ~previous_suspects[key]) != 0;
                    previous_suspects[key] = e.suspects;
                    const bool trigger = e.inferred_cut || e.marker != Marker::none ||
                        (e.kind == Kind::camera && e.camera.cut && e.camera.cut_known) || changed_suspects;
                    if (trigger) {
                        burst_until = e.time_ns + 5 * second;
                        for (auto& h : history) { if (!save(h)) { storage_full = true; break; } }
                    }
                    const bool critical = e.kind == Kind::marker || e.kind == Kind::gap;
                    if (!storage_full && (critical || e.time_ns <= burst_until ||
                        e.time_ns - (std::min)(e.time_ns, last_saved[key]) >= second / 30)) {
                        storage_full = !save(history.back());
                        last_saved[key] = e.time_ns;
                    }
                }
                const auto now = Recorder::clock_ns();
                if (now - last_flush >= second) {
                    const auto lost = dropped.load();
                    if (lost != reported_drops && !storage_full) {
                        Saved gap{};
                        gap.event.kind = Kind::gap;
                        gap.event.time_ns = now;
                        gap.event.session = session;
                        copy_text(gap.event.label, "queue contention/overflow; missing events = " + std::to_string(lost - reported_drops));
                        storage_full = !save(gap);
                        reported_drops = lost;
                    }
                    stream.flush();
                    if (!stream) { throw std::runtime_error("event flush failed"); }
                    last_flush = now;
                }
            }
            stream.flush();
            if (!stream) { throw std::runtime_error("final event flush failed"); }
            const Json summary{{"written", written.load()}, {"dropped", dropped.load()},
                {"suspect_camera_samples", suspects.load()}, {"probe_failures", probe_failures.load()},
                {"status", storage_full ? "storage_limit" : "stopped"}};
            const auto summary_text = summary.dump(2);
            if (bytes.load() + summary_text.size() <= limits.byte_limit) {
                std::ofstream summary_file(directory / "summary.json", std::ios::binary);
                summary_file << summary_text;
                summary_file.close();
                if (!summary_file) { throw std::runtime_error("summary write failed"); }
                bytes.fetch_add(summary_text.size());
            }
            if (!storage_full) { state.store(Status::off); }
        } catch (...) {
            state.store(Status::io_error);
        }
        enabled.store(false);
        done.store(true, std::memory_order_release);
    }
};

uint64_t Recorder::clock_ns() noexcept {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
Recorder::Recorder(Limits limits) : m_impl(std::make_unique<Impl>(limits)) {}
Recorder::~Recorder() {
    stop();
    if (m_impl->worker.joinable()) { m_impl->worker.join(); }
}
bool Recorder::start(const std::filesystem::path& root, std::string metadata) {
    auto& p = *m_impl;
    if (!p.done.load(std::memory_order_acquire)) { return false; }
    if (p.worker.joinable()) { p.worker.join(); }
    std::scoped_lock lock{p.mutex};
    p.queue.resize(p.limits.queue_capacity);
    p.head = p.count = 0;
    p.session = clock_ns();
    p.epoch = 1;
    p.sequence = p.cut_sequence = p.last_camera_queued = 0;
    p.world = 0;
    p.have_camera = p.have_neutral = p.previous_cut = false;
    p.last_view_time = {}; p.last_view_camera = {};
    p.last_projection_time = {}; p.last_projection_camera = {};
    p.written = p.dropped = p.suspects = p.bytes = p.probe_failures = 0;
    p.stop_requested.store(false);
    p.done.store(false);
    p.state.store(Status::starting);
    p.enabled.store(true);
    try { p.worker = std::thread{[&p, root, metadata = std::move(metadata)]() mutable { p.run(root, std::move(metadata)); }}; }
    catch (...) { p.enabled = false; p.done = true; p.state = Status::io_error; return false; }
    return true;
}
void Recorder::stop() noexcept {
    auto& p = *m_impl;
    p.enabled.store(false);
    p.stop_requested.store(true);
    auto state = p.state.load();
    while ((state == Status::recording || state == Status::starting) &&
        !p.state.compare_exchange_weak(state, Status::stopping)) {
    }
    p.wake.notify_one();
}
bool Recorder::active() const noexcept { return m_impl->enabled.load(std::memory_order_relaxed); }
Counters Recorder::counters() const noexcept {
    const auto& p = *m_impl;
    return {p.written.load(), p.dropped.load(), p.suspects.load(), p.bytes.load(), p.probe_failures.load(), p.state.load()};
}
void Recorder::camera(Camera c, uint32_t thread, float capture_us) noexcept {
    auto& p = *m_impl;
    if (!active()) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock || !active()) { p.dropped.fetch_add(1); return; }
    if (p.world != c.world || (p.have_camera && p.camera.camera.camera_manager != c.camera_manager)) {
        ++p.epoch; p.world = c.world; p.have_camera = p.have_neutral = false; p.previous_cut = false;
    }
    Event e{};
    e.time_ns = clock_ns(); e.thread = thread; e.kind = Kind::camera; e.camera = c; e.capture_us = capture_us;
    e.suspects = camera_suspects(c);
    e.inferred_cut = p.have_camera && trace::inferred_cut(p.camera.camera.input, c.input);
    const auto actual_cut = c.cut_known && c.cut && !p.previous_cut;
    p.previous_cut = c.cut_known && c.cut;
    e.camera.cut = actual_cut;
    if (!p.have_camera || e.inferred_cut || actual_cut) { ++p.cut_sequence; }
    if (p.have_camera && !e.inferred_cut && !actual_cut &&
        std::abs(c.focus_after - p.camera.camera.focus_after) > (std::max)(500.0f, c.focus_after * 0.4f)) {
        e.suspects |= abrupt_assist_change;
    }
    if (e.suspects) { p.suspects.fetch_add(1, std::memory_order_relaxed); }
    e.session = p.session; e.epoch = p.epoch; e.cut_sequence = p.cut_sequence;
    e.camera_sequence = ++p.sequence;
    e.sequence = e.camera_sequence;
    const bool edge = !p.have_camera || e.inferred_cut || actual_cut || e.suspects != p.camera.suspects;
    p.camera = e; p.have_camera = true;
    if (edge || e.time_ns - p.last_camera_queued >= second / 120) {
        p.push(e); p.last_camera_queued = e.time_ns;
    }
}
void Recorder::invalidate(uintptr_t world, std::string_view reason, uint32_t thread) noexcept {
    auto& p = *m_impl;
    if (!active()) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return; }
    if (!p.have_camera && p.world == world) { return; }
    ++p.epoch; p.world = world; p.have_camera = p.have_neutral = false;
    Event e{}; e.kind = Kind::gap; e.thread = thread; copy_text(e.label, reason); p.push(e);
}
Ticket Recorder::begin_view(Pose input, int32_t index, int32_t eye, uint32_t thread) noexcept {
    auto& p = *m_impl;
    if (!active()) { return {}; }
    // A skipped/unassociated offset must not reuse the preceding view's matrix tag.
    if (last_view_owner == this) { last_view_owner = nullptr; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return {}; }
    if (!active() || !p.have_camera) { return {}; }
    Ticket t{p.camera};
    t.event.kind = Kind::view; t.event.thread = thread; t.event.time_ns = clock_ns();
    t.event.view.input = input; t.event.view.input.fov = t.event.camera.input.fov;
    t.event.view.input.valid = finite(input.location) && finite(input.rotation);
    t.event.view.eye = eye; t.event.view.index = index;
    t.event.view.input_matches_assist = finite(input.location) && finite(input.rotation) &&
        distance(input.location, t.event.camera.input.location) < 100.0f &&
        angle_delta(input.rotation[0], t.event.camera.input.rotation[0]) < 5.0f &&
        angle_delta(input.rotation[1], t.event.camera.input.rotation[1]) < 5.0f;
    t.event.view.source_matches_input = source_matches_input(t.event.camera.native_source,
        t.event.view.input, t.event.camera.camera_manager, t.event.time_ns);
    t.event.view.snapshot_age_ms = (float)(t.event.time_ns - p.camera.time_ns) / 1'000'000.0f;
    if (eye < 0 || eye > 1) { return {}; }
    const auto channel = eye * 2 + (t.event.view.input_matches_assist ? 1 : 0);
    const bool critical = (p.camera.inferred_cut || p.camera.camera.cut) &&
        p.last_view_camera[channel] != p.camera.camera_sequence;
    if (!critical && t.event.time_ns - p.last_view_time[channel] < second / 120) { return {}; }
    p.last_view_time[channel] = t.event.time_ns;
    p.last_view_camera[channel] = p.camera.camera_sequence;
    return t;
}
void Recorder::finish_view(Ticket& t) noexcept {
    auto& p = *m_impl;
    if (!t || !active()) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return; }
    if (!active() || t.event.session != p.session || t.event.epoch != p.epoch || !p.have_camera) { return; }
    t.event.suspects |= view_suspects(t.event.camera, t.event.view);
    if (t.event.view.neutral_valid && t.event.view.input_matches_assist && t.event.view.eye == 0 && t.event.view.snapshot_age_ms <= 250.0f) {
        p.neutral = t.event; p.have_neutral = true;
    }
    last_view = t.event; last_view_owner = this;
    p.push(t.event);
}
void Recorder::projection(Matrix matrix, int32_t eye, uint32_t thread) noexcept {
    auto& p = *m_impl;
    if (!active() || eye < 0 || eye > 1 || last_view_owner != this || last_view.view.eye != eye ||
        clock_ns() - last_view.time_ns > second / 4) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return; }
    if (last_view.session != p.session || last_view.epoch != p.epoch) { return; }
    const auto now = clock_ns();
    const bool critical = (last_view.inferred_cut || last_view.camera.cut) &&
        p.last_projection_camera[eye] != last_view.camera_sequence;
    if (!critical && now - p.last_projection_time[eye] < second / 120) { return; }
    p.last_projection_time[eye] = now; p.last_projection_camera[eye] = last_view.camera_sequence;
    Event e = last_view; e.kind = Kind::projection; e.projection = matrix; e.thread = thread; e.time_ns = clock_ns();
    p.push(e);
}
void Recorder::observation(Observation sample, uint32_t thread) noexcept {
    auto& p = *m_impl;
    if (!active()) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return; }
    if (!p.have_camera || (sample.reference_camera && sample.reference_camera != p.camera.camera_sequence &&
        (!p.have_neutral || sample.reference_camera != p.neutral.camera_sequence))) { return; }
    // An older neutral transaction must not be relabelled as the newest camera/cut.
    Event e = sample.reference_camera && sample.reference_camera != p.camera.camera_sequence ? p.neutral : p.camera;
    e.kind = Kind::observation; e.thread = thread; e.time_ns = clock_ns(); e.observation = sample;
    p.push(e);
}
void Recorder::mark(Marker marker, std::string_view label, uint32_t thread) noexcept {
    auto& p = *m_impl;
    if (!active()) { return; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock) { p.dropped.fetch_add(1); return; }
    Event e = p.have_camera ? p.camera : Event{};
    e.kind = Kind::marker; e.thread = thread; e.marker = marker; e.time_ns = clock_ns(); copy_text(e.label, label); p.push(e);
}
std::optional<Event> Recorder::latest_camera() noexcept {
    auto& p = *m_impl;
    if (!active()) { return {}; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock || !active() || !p.have_camera) { return {}; }
    return p.camera;
}
std::optional<Event> Recorder::latest_neutral() noexcept {
    auto& p = *m_impl;
    if (!active()) { return {}; }
    std::unique_lock lock{p.mutex, std::try_to_lock};
    if (!lock || !active() || !p.have_neutral) { return {}; }
    return p.neutral;
}
void Recorder::probe_failed() noexcept { m_impl->probe_failures.fetch_add(1, std::memory_order_relaxed); }
const char* status_name(Status s) noexcept {
    static constexpr const char* names[] = {"Off", "Starting", "Recording", "Stopping", "Storage limit reached", "I/O error"};
    return names[(int)s];
}
}
