#include "PacingRecorder.hpp"

#include <Windows.h>

namespace utility::pacing {
namespace { thread_local Owner thread_owner{Owner::normal}; }
Recorder<>& recorder() noexcept { static Recorder<> value; return value; }
Owner wait_owner() noexcept { return thread_owner; }
ScopedOwner::ScopedOwner(Owner owner) noexcept : m_previous{thread_owner} { thread_owner = owner; }
ScopedOwner::~ScopedOwner() { thread_owner = m_previous; }

ScopedEvent::ScopedEvent(Stage stage, Domain domain, uint32_t frame) noexcept
    : m_token{recorder().control()} {
    if (!m_token) { return; }
    m_event.stage = stage;
    m_event.domain = domain;
    m_event.frame = frame;
    m_event.owner = wait_owner();
    m_event.thread = GetCurrentThreadId();
    m_event.start_ns = now_ns();
}
void ScopedEvent::finish() noexcept {
    if (!m_token) { return; }
    m_event.duration_ns = now_ns() - m_event.start_ns;
    recorder().record(m_token, m_event);
    m_token = 0;
}
void ScopedEvent::finish_at(uint64_t end_ns, int32_t result) noexcept {
    if (!m_token) { return; }
    m_event.duration_ns = end_ns >= m_event.start_ns ? end_ns - m_event.start_ns : 0;
    m_event.result = result;
    m_event.result_known = true;
    recorder().record(m_token, m_event);
    m_token = 0;
}
void decision(Reason reason, Context context) noexcept {
    ScopedEvent event{Stage::wait_decision, context.next_frame ? Domain::next_engine : Domain::unknown, context.next_frame};
    event.data().reason = reason;
    event.data().runtime_id = context.runtime_id;
    event.data().session_generation = context.session_generation;
    event.data().wait_sequence = context.wait_sequence;
    event.data().display_time = context.display_time;
}

const char* name(Stage value) noexcept {
    switch (value) {
    case Stage::wait_frame: return "xrWaitFrame";
    case Stage::begin_frame: return "xrBeginFrame";
    case Stage::end_frame: return "xrEndFrame";
    case Stage::wait_decision: return "wait_decision";
    case Stage::present: return "uevr_present_cpu";
    case Stage::post_present: return "uevr_post_present_cpu";
    case Stage::d3d11_frame: return "uevr_d3d11_frame_cpu";
    case Stage::d3d12_frame: return "uevr_d3d12_frame_cpu";
    case Stage::ui_copy: return "ui_copy_recording_cpu";
    case Stage::scene_copy: return "scene_copy_recording_cpu";
    case Stage::spectator: return "spectator_recording_cpu";
    case Stage::marker: return "user_marker";
    case Stage::wait_poses: return "openvr_WaitGetPoses";
    default: return "unknown";
    }
}
const char* name(Owner value) noexcept {
    switch (value) {
    case Owner::wait_only_worker: return "wait_only_worker";
    case Owner::array_worker: return "array_preacquire_worker";
    case Owner::normal_consumer: return "normal_consumer_fulfilling_reservation";
    default: return "normal";
    }
}
const char* name(Domain value) noexcept {
    switch (value) {
    case Domain::engine: return "engine_frame_argument";
    case Domain::xr_internal: return "openxr_internal_frame";
    case Domain::uevr_render: return "uevr_render_frame";
    case Domain::next_engine: return "next_engine_frame_candidate";
    default: return "unavailable";
    }
}
const char* name(Reason value) noexcept {
    switch (value) {
    case Reason::frame_lock_busy: return "frame_lock_busy";
    case Reason::assignment_busy: return "pose_assignment_busy";
    case Reason::first_frame: return "first_valid_submission_required";
    case Reason::normal_pacing: return "frame_boundary_not_admissible";
    case Reason::pose_ahead: return "next_pose_or_retained_slot_evidence";
    case Reason::reserved: return "next_wait_reserved";
    case Reason::reservation_busy: return "reservation_unavailable";
    case Reason::worker_busy: return "existing_async_request_pending_or_running";
    case Reason::cancelled: return "queued_work_cancelled";
    case Reason::stale_request: return "stale_request";
    case Reason::exception: return "exception";
    case Reason::reinitializing: return "runtime_reinitialization_requested";
    case Reason::valid_poses_required: return "first_valid_poses_required";
    case Reason::submit_frame_required: return "submitted_frame_identity_required";
    case Reason::frame_already_began: return "frame_already_began";
    case Reason::frame_already_synced: return "frame_already_synchronized";
    default: return "none";
    }
}
}
