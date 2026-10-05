#define NOMINMAX
#include "ProSpiCameraProbe.hpp"
#include <Windows.h>
#include <sdk/UObjectArray.hpp>
#include <sdk/UClass.hpp>
#include <sdk/FStructProperty.hpp>
#include <sdk/FBoolProperty.hpp>

namespace uevr::prospi::trace {
namespace {
bool type(sdk::FProperty* p, std::wstring_view expected) {
    return p && p->get_class() && p->get_class()->get_name().to_string() == expected;
}
int offset(sdk::UStruct* owner, const wchar_t* name, std::wstring_view expected, int bytes) {
    const auto p = owner->find_property(name);
    if (!type(p, expected)) { return -1; }
    const auto n = p->get_offset();
    return n >= 0 && n + bytes <= owner->get_properties_size() ? n : -1;
}
bool read_memory(uintptr_t source, void* destination, size_t size) {
    SIZE_T copied{};
    return ReadProcessMemory(GetCurrentProcess(), (void*)source, destination, size, &copied) && copied == size;
}
}
void CameraProbe::reset() noexcept {
    m_identity.reset(); m_class = 0; m_cache = -1; m_last_post = 0; m_previous_post_cut = false;
}
bool CameraProbe::resolve(sdk::UObjectBase* pcm) {
    const auto cls = pcm->get_class();
    if (!cls) { return false; }
    if ((uintptr_t)cls == m_class) { return m_cache >= 0; }
    m_cache = -1;
    m_cut = m_target = m_aspect = m_timestamp = -1;
    auto cache = cls->find_property(L"CameraCachePrivate");
    if (!cache) { cache = cls->find_property(L"CameraCache"); }
    if (!type(cache, L"StructProperty")) { return false; }
    const auto structure = ((sdk::FStructProperty*)cache)->get_struct();
    if (!structure) { return false; }
    const auto pov = structure->find_property(L"POV");
    if (!type(pov, L"StructProperty")) { return false; }
    const auto info = ((sdk::FStructProperty*)pov)->get_struct();
    if (!info) { return false; }
    const auto location = offset(info, L"Location", L"StructProperty", 12);
    const auto rotation = offset(info, L"Rotation", L"StructProperty", 12);
    const auto fov = offset(info, L"FOV", L"FloatProperty", 4);
    // Only the float-vector UE4 layout established in the ProSpi dump is accepted.
    const auto lp = (sdk::FStructProperty*)info->find_property(L"Location");
    const auto rp = (sdk::FStructProperty*)info->find_property(L"Rotation");
    if (location < 0 || rotation < 0 || fov < 0 || !lp->get_struct() || !rp->get_struct() ||
        lp->get_struct()->get_properties_size() != 12 || rp->get_struct()->get_properties_size() != 12) { return false; }
    m_location = pov->get_offset() + location;
    m_rotation = pov->get_offset() + rotation;
    m_fov = pov->get_offset() + fov;
    const auto aspect = offset(info, L"AspectRatio", L"FloatProperty", 4);
    m_aspect = aspect >= 0 ? pov->get_offset() + aspect : -1;
    m_timestamp = offset(structure, L"TimeStamp", L"FloatProperty", 4);
    m_cache_bytes = (std::max)({m_location + 12, m_rotation + 12, m_fov + 4, m_aspect + 4, m_timestamp + 4});
    if (m_cache_bytes > 512 || m_cache_bytes > structure->get_properties_size() || cache->get_offset() < 0 ||
        cache->get_offset() + m_cache_bytes > cls->get_properties_size()) { return false; }
    const auto cut = cls->find_property(L"bGameCameraCutThisFrame");
    if (type(cut, L"BoolProperty")) {
        const auto b = (sdk::FBoolProperty*)cut;
        const auto n = b->get_offset() + b->get_byte_offset();
        if (n >= 0 && n < cls->get_properties_size() && b->get_byte_mask() != 0) {
            m_cut = n; m_cut_mask = b->get_byte_mask();
        }
    }
    const auto target = cls->find_property(L"ViewTarget");
    if (type(target, L"StructProperty")) {
        const auto target_struct = ((sdk::FStructProperty*)target)->get_struct();
        if (target_struct) {
            const auto n = offset(target_struct, L"Target", L"ObjectProperty", 8);
            if (n >= 0 && target->get_offset() >= 0 && target->get_offset() + n + 8 <= cls->get_properties_size()) {
                m_target = target->get_offset() + n;
            }
        }
    }
    m_cache = cache->get_offset();
    m_class = (uintptr_t)cls;
    return true;
}
bool CameraProbe::read(Pose& pose, bool& cut_known, bool& cut, uintptr_t& target) const {
    if (!m_identity || m_cache < 0 || !sdk::is_current_object(*m_identity)) { return false; }
    std::array<uint8_t, 512> data{};
    if (!read_memory(m_identity->object + m_cache, data.data(), m_cache_bytes)) { return false; }
    std::memcpy(pose.location.data(), data.data() + m_location, 12);
    std::memcpy(pose.rotation.data(), data.data() + m_rotation, 12);
    std::memcpy(&pose.fov, data.data() + m_fov, 4);
    if (m_aspect >= 0) {
        std::memcpy(&pose.aspect, data.data() + m_aspect, 4);
        pose.aspect_valid = std::isfinite(pose.aspect) && pose.aspect > 0 && pose.aspect < 10;
    }
    if (m_timestamp >= 0) {
        std::memcpy(&pose.cache_timestamp, data.data() + m_timestamp, 4);
        pose.timestamp_valid = std::isfinite(pose.cache_timestamp);
    }
    uint8_t bits{};
    cut_known = m_cut >= 0 && read_memory(m_identity->object + m_cut, &bits, 1);
    cut = cut_known && (bits & m_cut_mask) != 0;
    target = 0;
    if (m_target >= 0) { read_memory(m_identity->object + m_target, &target, 8); }
    pose.valid = finite(pose.location) && finite(pose.rotation);
    return sdk::is_current_object(*m_identity) && valid_pose(pose);
}
bool CameraProbe::capture(sdk::UObjectBase* pcm, Camera& c) noexcept {
    try {
        if (!sdk::UObjectBase::has_validated_header_layout()) { return false; }
        const auto observed = sdk::observe_uobject(pcm);
        if (!observed || !sdk::is_current_object(observed->identity)) { return false; }
        m_identity = observed->identity;
        if (!resolve(pcm)) { return false; }
        Pose p{};
        if (!read(p, c.cut_known, c.cut, c.view_target)) { return false; }
        // Keep the original local values used by the assist; additional reads are diagnostics only.
        c.input.aspect = p.aspect; c.input.aspect_valid = p.aspect_valid;
        c.input.cache_timestamp = p.cache_timestamp; c.input.timestamp_valid = p.timestamp_valid;
        return true;
    } catch (...) { reset(); return false; }
}
void CameraProbe::post_tick(Recorder& recorder, uint32_t thread) noexcept {
    if (!recorder.active()) { return; }
    const auto now = Recorder::clock_ns();
    if (now - m_last_post < 1'000'000'000 / 120) { return; }
    m_last_post = now;
    try {
        const auto camera = recorder.latest_camera();
        if (!camera || !m_identity || camera->camera.camera_manager != m_identity->object ||
            now - camera->time_ns > 250'000'000) { return; }
        Observation o{};
        uintptr_t target{};
        if (!read(o.cache, o.cut_known, o.cut, target)) { recorder.probe_failed(); return; }
        o.reference_camera = camera->camera_sequence;
        recorder.observation(o, thread);
        if (o.cut_known && o.cut && !m_previous_post_cut) {
            recorder.mark(Marker::automatic, "game cut flag observed after engine tick", thread);
        }
        m_previous_post_cut = o.cut_known && o.cut;
    } catch (...) { recorder.probe_failed(); reset(); }
}
}
