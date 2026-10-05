#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace uevr::prospi::trace {

using Vector = std::array<float, 3>;
using Matrix = std::array<float, 16>;

template <size_t N>
void copy_text(std::array<char, N>& destination, std::string_view source) noexcept {
    static_assert(N > 0);
    const auto size = (std::min)(source.size(), N - 1);
    if (size != 0) { std::memcpy(destination.data(), source.data(), size); }
    destination[size] = '\0';
}

inline bool finite(const Vector& v) noexcept {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline float distance(const Vector& a, const Vector& b) noexcept {
    const auto x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}
inline float angle_delta(float a, float b) noexcept {
    return std::abs(std::remainder(a - b, 360.0f));
}

struct Pose {
    Vector location{}, rotation{};
    float fov{}, aspect{}, cache_timestamp{};
    bool valid{}, aspect_valid{}, timestamp_valid{};
};

struct Match {
    std::array<char, 96> id{};
    float score{}, confidence{}, weight{}, focus_distance{}, min_fov{}, multiplier{};
};

enum class Provenance : uint8_t { unavailable, pre_tick_cache, post_tick_cache, stereo_input };
enum class Kind : uint8_t { camera, view, projection, observation, marker, gap };
enum class Marker : uint8_t { none, bad, good, automatic };
enum class Status : uint8_t { off, starting, recording, stopping, storage_limit, io_error };
enum Suspect : uint32_t {
    invalid_camera = 1u << 0,
    ambiguous_match = 1u << 1,
    large_safety_lift = 1u << 2,
    estimated_floor_unmet = 1u << 3,
    safety_estimate_disagrees = 1u << 4,
    target_behind = 1u << 5,
    target_offscreen = 1u << 6,
    dolly_past_target = 1u << 7,
    static_path_obstruction = 1u << 8,
    abrupt_assist_change = 1u << 9,
    stale_assist_observation = 1u << 10,
};

struct Camera {
    Pose input{};
    uintptr_t world{}, camera_manager{}, view_target{};
    std::array<char, 96> id{}, dolly_source{};
    std::array<Match, 3> matches{};
    uint32_t match_count{};
    int32_t preset{}, mode{}, play_mode{}, source{}, zone{}, rendering_method{};
    int32_t framing_status{};
    float framing_candidate_focus{}, framing_candidate_dolly{};
    float confidence{}, base_fov{}, effective_fov{}, min_fov{}, multiplier{1.0f};
    float focus_before{}, focus_after{}, dolly_before{}, dolly_after{};
    float safety_min_z{}, safety_predicted_z{}, safety_up{}, safety_max_up{};
    float camera_forward{}, camera_right{}, camera_up{}, smoothing{};
    bool calibration_applied{}, wrote_fov{}, previous_update_wrote_fov{};
    bool cut_known{}, cut{}, safety_active{}, dolly_enabled{}, stabilizer{}, segment_latch{}, sequencer_active{};
    Provenance provenance{};
};

struct View {
    Pose input{}, neutral{}, output{};
    Vector forward_offset{}, right_offset{}, up_offset{};
    float world_to_meters{}, world_scale{}, snapshot_age_ms{};
    int32_t index{}, eye{-1};
    bool neutral_valid{}, decoupled_pitch{}, full_pass{}, input_matches_assist{};
};

struct Surface {
    Vector point{}, normal{};
    float distance{};
    uintptr_t component{};
    bool query_valid{}, hit{}, static_component{};
};

struct Observation {
    Pose cache{}, source_camera{};
    Vector ball{}, ball_velocity{}, subject{};
    Surface floor{}, dolly_path{};
    uintptr_t ball_object{}, subject_object{};
    uint64_t reference_camera{};
    std::array<char, 96> stadium{};
    bool ball_valid{}, velocity_valid{}, subject_valid{}, replay_known{}, replay{};
    bool source_camera_valid{}, geometry_available{}, cut_known{}, cut{};
};

struct Event {
    uint64_t sequence{}, time_ns{}, session{}, epoch{}, camera_sequence{}, cut_sequence{};
    uint32_t thread{}, suspects{};
    Kind kind{};
    Marker marker{};
    Camera camera{};
    View view{};
    Matrix projection{};
    Observation observation{};
    std::array<char, 128> label{};
    float capture_us{};
    bool inferred_cut{};
};
static_assert(std::is_trivially_copyable_v<Event>);

inline bool valid_pose(const Pose& p) noexcept {
    return p.valid && finite(p.location) && finite(p.rotation) &&
        std::isfinite(p.fov) && p.fov > 0.01f && p.fov < 179.0f;
}

// These are evidence markers, not instructions to move the camera.
inline uint32_t camera_suspects(const Camera& c) noexcept {
    if (!valid_pose(c.input)) { return invalid_camera; }
    uint32_t flags{};
    if ((c.mode == 2 || c.mode == 3) && c.source == 3 && c.match_count > 1) {
        const auto& a = c.matches[0];
        const auto& b = c.matches[1];
        const auto largest = (std::max)(a.focus_distance, b.focus_distance);
        if (largest > 1.0f && std::abs(a.focus_distance - b.focus_distance) > largest * 0.4f &&
            std::abs(a.confidence - b.confidence) < 0.1f) { flags |= ambiguous_match; }
    }
    if (c.safety_active && c.safety_up > 75.0f) { flags |= large_safety_lift; }
    if (c.safety_active && c.safety_predicted_z + c.safety_up < c.safety_min_z - 2.0f) {
        flags |= estimated_floor_unmet;
    }
    return flags;
}

inline uint32_t view_suspects(const Camera& c, const View& v) noexcept {
    uint32_t flags{};
    if (v.snapshot_age_ms > 250.0f) { flags |= stale_assist_observation; }
    if (v.neutral_valid && c.safety_active && !v.decoupled_pitch &&
        std::abs(v.input.location[2] - c.input.location[2]) < 20.0f) {
        // A camera-local lift need not equal a world-Z lift. Compare measured output.
        const auto estimate = c.safety_predicted_z + c.safety_up;
        if (std::abs(v.neutral.location[2] - estimate) > 75.0f) {
            flags |= safety_estimate_disagrees;
        }
    }
    return flags;
}

inline bool inferred_cut(const Pose& before, const Pose& after) noexcept {
    return valid_pose(before) && valid_pose(after) &&
        (distance(before.location, after.location) > 750.0f ||
         angle_delta(before.rotation[1], after.rotation[1]) > 30.0f ||
         angle_delta(before.rotation[0], after.rotation[0]) > 20.0f ||
         std::abs(before.fov - after.fov) > 8.0f);
}

struct Ticket {
    Event event{};
    explicit operator bool() const noexcept { return event.camera_sequence != 0; }
};

struct Counters {
    uint64_t written{}, dropped{}, suspects{}, bytes{}, probe_failures{};
    Status status{};
};

struct Limits {
    size_t queue_capacity{2048}, history_capacity{4096};
    uint64_t byte_limit{512ull * 1024 * 1024};
};

class Recorder {
public:
    explicit Recorder(Limits limits = {});
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool start(const std::filesystem::path& root, std::string metadata_json);
    void stop() noexcept;
    bool active() const noexcept;
    Counters counters() const noexcept;
    void camera(Camera sample, uint32_t thread, float capture_us = 0.0f) noexcept;
    void invalidate(uintptr_t world, std::string_view reason, uint32_t thread) noexcept;
    Ticket begin_view(Pose input, int32_t index, int32_t eye, uint32_t thread) noexcept;
    void finish_view(Ticket& ticket) noexcept;
    void projection(Matrix matrix, int32_t eye, uint32_t thread) noexcept;
    void observation(Observation sample, uint32_t thread) noexcept;
    void mark(Marker marker, std::string_view label, uint32_t thread) noexcept;
    std::optional<Event> latest_camera() noexcept;
    std::optional<Event> latest_neutral() noexcept;
    void probe_failed() noexcept;

    static uint64_t clock_ns() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

const char* status_name(Status status) noexcept;

}
