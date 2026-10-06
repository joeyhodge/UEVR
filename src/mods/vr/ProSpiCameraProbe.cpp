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
bool native_layout(native::Layout& layout) {
    layout.base = (uintptr_t)GetModuleHandleW(nullptr);
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read_memory(layout.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || dos.e_lfanew > 4096 ||
        !read_memory(layout.base + dos.e_lfanew, &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.TimeDateStamp != 1786532279 || nt.OptionalHeader.SizeOfImage != 783106048) { return false; }
    layout.image_size = nt.OptionalHeader.SizeOfImage;
    struct Fingerprint { uint32_t rva; std::string_view bytes; };
    static constexpr Fingerprint fingerprints[] = {
        {0x05819570, "488bc44889580848897010574881ecc00000000f2970e80f2978d8440f2940c8"},
        {0x058195e0, "4885db480f44d84038b0140400007405488bd8eb08488bc8e8c3010000488bcf"},
        {0x05819616, "440f285370440f295424300f2883800000000f29442420f3410f5cc20f28d045"},
        {0x05811410, "dfe85a810000e8f5890100b801000000f00fc1051873790d8b0d1273790d908b"},
        {0x058170e0, "40534883ec20488bd9488d0d6aa2d514e84bedb101488b430833c9488b400848"},
        {0x05818b20, "40534883ec20488bd9488d0ddd88d514e80bd3b101488bc34883c4205bc3cccc"},
        {0x05817000, "4883ec28488d0d4fa3d514e830eeb10133c04883c428c3cccccccccccccccccc"},
        {0x05811e80, "4883ec28488d0dd2f3d514e8b03fb201488b058970780d488b40104883c428c3"},
        {0x030ddb50, "4883ec48f20f101a8b42080f28e3f30f100d0e6578040f28c389442438f30f10"},
        {0x030dd3b0, "40534883ec50f20f1012488bd98b4208488d4c24300f28ca89442448f30f1044"},
        {0x030dd850, "48895c2408574883ec30488b5910488bf90f297424204885db7411488b832802"},
    };
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (const auto& f : fingerprints) {
        std::array<uint8_t, 32> actual{};
        if (f.bytes.size() != actual.size() * 2 || !native::module(layout, layout.base + f.rva, actual.size()) ||
            !read_memory(layout.base + f.rva, actual.data(), actual.size())) { return false; }
        for (size_t i = 0; i < actual.size(); ++i) {
            if (actual[i] != ((digit(f.bytes[i * 2]) << 4) | digit(f.bytes[i * 2 + 1]))) { return false; }
        }
    }
    return true;
}
}
void CameraProbe::reset() noexcept {
    m_identity.reset(); m_class = 0; m_cache = -1; m_last_post = 0; m_previous_post_cut = false;
    m_native_layout = {}; m_native_attempted = m_native_supported = false;
}
NativeSource CameraProbe::capture_native(uintptr_t pcm) noexcept {
    const auto started = Recorder::clock_ns();
    // Only the recorder or the opt-in focus guard enters this read-only path.
    if (!m_native_attempted) {
        m_native_attempted = true;
        m_native_supported = native_layout(m_native_layout);
    }
    NativeSource source{};
    if (!m_native_supported) { source.status = SourceStatus::unsupported_layout; return source; }
    source = native::sample(read_memory, m_native_layout, pcm, Recorder::clock_ns());
    source.capture_us = (float)(Recorder::clock_ns() - started) / 1000.0f;
    return source;
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
        o.native_source = capture_native(m_identity->object);
        o.source_camera_valid = valid_source(o.native_source);
        if (o.source_camera_valid) { o.source_camera = o.native_source.pose; }
        o.reference_camera = camera->camera_sequence;
        recorder.observation(o, thread);
        if (o.cut_known && o.cut && !m_previous_post_cut) {
            recorder.mark(Marker::automatic, "game cut flag observed after engine tick", thread);
        }
        m_previous_post_cut = o.cut_known && o.cut;
    } catch (...) { recorder.probe_failed(); reset(); }
}
}
