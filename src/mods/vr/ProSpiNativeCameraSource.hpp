#pragma once
#include "ProSpiCameraTrace.hpp"

namespace uevr::prospi::trace::native {

struct Layout {
    uintptr_t base{};
    size_t image_size{};
    uint32_t context_rva{0x12f98f20}, sentinel_rva{0x12f994e8}, frame_rva{0x12fa8740};
    uint32_t sentinel_vtable_rva{0x083be008}, bridge_vtable_rva{0x07a4e7c0};
    uint32_t camera_getter_rva{0x05818b20}, empty_getter_rva{0x05817000};
};

inline bool pointer(uintptr_t p) noexcept {
    return p >= 0x10000 && p < 0x0000800000000000ull && (p & 7) == 0;
}
inline bool module(const Layout& l, uintptr_t p, size_t n = 8) noexcept {
    return p >= l.base && p - l.base < l.image_size && n <= l.image_size - (p - l.base);
}

// No allocation, game calls, writes or unbounded walks. Read witnesses are rechecked
// before accepting a sample; the native DWORD frame counter is only another witness.
template <typename Read>
class Snapshot {
public:
    Snapshot(Read& read) : m_read(read) {}
    bool bytes(uintptr_t address, void* destination, size_t size) noexcept {
        if (m_count == m_entries.size() || size > 64 || size == 0 ||
            address < 0x10000 || address > 0x0000800000000000ull - size) { return false; }
        auto& e = m_entries[m_count];
        if (!m_read(address, e.bytes.data(), size)) { return false; }
        e.address = address; e.size = size;
        std::memcpy(destination, e.bytes.data(), size);
        ++m_count;
        return true;
    }
    template <typename T> bool value(uintptr_t address, T& out) noexcept {
        return bytes(address, &out, sizeof(out));
    }
    bool ptr(uintptr_t address, uintptr_t& out) noexcept {
        return value(address, out) && pointer(out);
    }
    bool verify() noexcept {
        std::array<uint8_t, 64> data{};
        for (size_t i = 0; i < m_count; ++i) {
            const auto& e = m_entries[i];
            if (!m_read(e.address, data.data(), e.size) ||
                std::memcmp(e.bytes.data(), data.data(), e.size) != 0) { return false; }
        }
        return true;
    }
private:
    struct Witness { uintptr_t address{}; size_t size{}; std::array<uint8_t, 64> bytes{}; };
    std::array<Witness, 300> m_entries{};
    size_t m_count{};
    Read& m_read;
};

template <typename Read>
NativeSource sample(Read read, const Layout& layout, uintptr_t expected_pcm, uint64_t time_ns) noexcept {
    NativeSource result{};
    const auto reject = [&](SourceStatus status) { NativeSource empty{}; empty.status = status; return empty; };
    if (!pointer(layout.base) || !module(layout, layout.base + layout.frame_rva, 4) ||
        !module(layout, layout.base + layout.context_rva) || !module(layout, layout.base + layout.sentinel_rva)) {
        return reject(SourceStatus::unsupported_layout);
    }
    Snapshot<Read> s{read};
    uintptr_t context{}, sentinel{}, bridge_interface{}, vt{}, actor{}, component{};
    if (!s.value(layout.base + layout.frame_rva, result.frame) ||
        !s.ptr(layout.base + layout.context_rva, context) ||
        !s.ptr(layout.base + layout.sentinel_rva, sentinel) || !s.ptr(context + 0x10, bridge_interface) ||
        !s.ptr(bridge_interface, vt)) { return reject(SourceStatus::unreadable); }
    if (vt != layout.base + layout.bridge_vtable_rva) { return reject(SourceStatus::unsupported_layout); }
    if (!s.ptr(bridge_interface + 8, result.camera_manager)) { return reject(SourceStatus::unreadable); }
    if (result.camera_manager != expected_pcm) { return reject(SourceStatus::manager_mismatch); }
    if (!s.ptr(bridge_interface + 0x10, actor) || !s.ptr(actor + 0x228, component) ||
        !s.ptr(component, vt) || !module(layout, vt)) { return reject(SourceStatus::unreadable); }
    std::array<uint8_t, 48> bridge{};
    float aspect{};
    if (!s.bytes(bridge_interface + 0x140, bridge.data(), bridge.size()) ||
        !s.value(component + 0x208, aspect) || !s.ptr(sentinel, vt)) { return reject(SourceStatus::unreadable); }
    if (vt != layout.base + layout.sentinel_vtable_rva) { return reject(SourceStatus::unsupported_layout); }
    uintptr_t sentinel_metadata{};
    std::array<uintptr_t, 2> ends{};
    if (!s.ptr(sentinel + 8, sentinel_metadata) || !s.value(sentinel_metadata, ends) ||
        !pointer(ends[0]) || !pointer(ends[1])) { return reject(SourceStatus::unreadable); }
    std::array<uintptr_t, 64> visited{};
    auto previous = sentinel, node = ends[1];
    while (node != sentinel) {
        if (result.list_nodes == visited.size() ||
            std::find(visited.begin(), visited.begin() + result.list_nodes, node) != visited.begin() + result.list_nodes) {
            return reject(SourceStatus::corrupt_list);
        }
        visited[result.list_nodes++] = node;
        std::array<uintptr_t, 2> header{}, links{};
        uintptr_t getter{};
        if (!s.value(node, header) || !module(layout, header[0], 0x40) || !pointer(header[1]) ||
            !s.value(header[1], links) || !pointer(links[1]) || !s.value(header[0] + 0x38, getter)) {
            return reject(SourceStatus::unreadable);
        }
        if (links[0] != previous) { return reject(SourceStatus::corrupt_list); }
        if (getter == layout.base + layout.camera_getter_rva) {
            uint8_t selected{};
            if (!s.value(node + 0x414, selected)) { return reject(SourceStatus::unreadable); }
            // Mirrors the proven native publisher: first fallback, LAST nonzero selector.
            if (!result.object || selected) { result.object = node; }
        } else if (getter != layout.base + layout.empty_getter_rva) {
            return reject(SourceStatus::unsupported_node);
        }
        previous = node; node = links[1];
    }
    if (previous != ends[0]) { return reject(SourceStatus::corrupt_list); }
    if (!result.object) { return reject(SourceStatus::unavailable); }
    std::array<uint8_t, 64> camera{};
    uintptr_t lens{};
    float vfov{};
    if (!s.bytes(result.object + 0x60, camera.data(), camera.size())) { return reject(SourceStatus::unreadable); }
    std::memcpy(&lens, camera.data(), 8);
    if (!pointer(lens) || !s.value(lens + 0x1c, vfov)) { return reject(SourceStatus::unreadable); }
    Vector native_position{}, native_target{}, bridge_position{};
    std::array<float, 4> q{};
    float bridge_fov{};
    std::memcpy(native_position.data(), camera.data() + 0x10, 12);
    std::memcpy(native_target.data(), camera.data() + 0x20, 12);
    std::memcpy(bridge_position.data(), bridge.data(), 12);
    std::memcpy(q.data(), bridge.data() + 0x10, 16);
    std::memcpy(&bridge_fov, bridge.data() + 0x20, 4);
    if (!finite(native_position) || !finite(native_target) || !finite(bridge_position) ||
        !std::all_of(q.begin(), q.end(), [](float x) { return std::isfinite(x); }) ||
        !std::isfinite(vfov) || vfov <= 0.0001f || vfov >= 3.14149f ||
        !std::isfinite(aspect) || aspect <= 0.1f || aspect >= 10 || !std::isfinite(bridge_fov)) {
        return reject(SourceStatus::invalid_values);
    }
    const auto qlength = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
    if (std::abs(qlength - 1.0f) > 0.01f) { return reject(SourceStatus::invalid_values); }
    result.pose.location = {native_position[0] * 10, native_position[2] * 10, native_position[1] * 10};
    result.look_at = {native_target[0] * 10, native_target[2] * 10, native_target[1] * 10};
    Vector delta{};
    for (size_t i = 0; i < 3; ++i) { delta[i] = result.look_at[i] - result.pose.location[i]; }
    result.focus_distance = distance(result.look_at, result.pose.location);
    if (!std::isfinite(result.focus_distance) || result.focus_distance <= 0.01f || result.focus_distance >= 1e8f) {
        return reject(SourceStatus::invalid_values);
    }
    constexpr float degrees = 57.29577951308232f;
    result.pose.rotation = {std::atan2(delta[2], std::hypot(delta[0], delta[1])) * degrees,
        std::atan2(delta[1], delta[0]) * degrees, 0};
    result.pose.fov = 2 * std::atan(std::tan(vfov * 0.5f) * aspect) * degrees;
    result.pose.aspect = aspect; result.pose.aspect_valid = true; result.pose.valid = true;
    const Vector bridge_forward{1 - 2*(q[1]*q[1] + q[2]*q[2]),
        2*(q[0]*q[1] + q[3]*q[2]), 2*(q[0]*q[2] - q[3]*q[1])};
    const auto cosine = dot(delta, bridge_forward) /
        (result.focus_distance * std::sqrt(dot(bridge_forward, bridge_forward)));
    if (!std::isfinite(cosine) || !valid_pose(result.pose)) { return reject(SourceStatus::invalid_values); }
    result.position_error = distance(result.pose.location, bridge_position);
    result.direction_error = std::acos(std::clamp(cosine, -1.0f, 1.0f)) * degrees;
    result.fov_error = std::abs(result.pose.fov - bridge_fov);
    uint32_t frame_after{};
    if (!s.verify() || !read(layout.base + layout.frame_rva, &frame_after, 4) || frame_after != result.frame) {
        return reject(SourceStatus::changed_during_read);
    }
    if (result.position_error > 0.25f || result.direction_error > 0.1f || result.fov_error > 0.01f) {
        return reject(SourceStatus::bridge_mismatch);
    }
    result.camera_actor = actor; result.time_ns = time_ns; result.status = SourceStatus::accepted;
    return result;
}

}
