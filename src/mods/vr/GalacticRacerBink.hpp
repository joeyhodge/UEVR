#pragma once

#include "GalacticRacerBinkCode.hpp"
#include "GalacticRacerRenderTargets.hpp"

namespace uevr::swgr_bink {
inline constexpr size_t packet_boundary = 0xfc;
inline constexpr const char* overlay_signature =
    "56 57 48 83 EC 58 48 8B 05 ? ? ? ? 48 31 E0 48 89 44 24 50 8B 41 04";

inline bool in_module(uintptr_t p, size_t n, uintptr_t base, size_t size) {
    return p >= base && p - base <= size && n <= size - (p - base);
}
inline std::optional<uintptr_t> call_target(const sdk::discovery::Memory& m, uintptr_t ip,
    uintptr_t base, size_t size) {
    std::array<uint8_t, 5> call{};
    int32_t displacement{};
    if (!in_module(ip, call.size(), base, size) || !m.load(ip, call) || call[0] != 0xe8) { return {}; }
    std::memcpy(&displacement, call.data() + 1, sizeof(displacement));
    const auto target = sdk::discovery::relative_address(ip + call.size(), displacement);
    if (!target || !in_module(*target, 1, base, size) || !m.executable ||
        !m.executable(m.context, *target, 1)) { return {}; }
    return target;
}

// Prove the whole viewport callback, its local packet ABI, and the Bink
// consumers. No RVAs, filename-based movie list, or generic texture hook.
inline bool overlay_contract(const sdk::discovery::Memory& m, uintptr_t begin,
    size_t unwind_size, uintptr_t base, size_t size) {
    if (unwind_size != overlay_code.size() || !in_module(begin, unwind_size, base, size) ||
        !swgr::code_matches(m, begin, overlay_code, overlay_mask)) { return false; }
    const auto writer = call_target(m, begin + 0xff, base, size);
    const auto scheduled = call_target(m, begin + 0x104, base, size);
    const auto draw = call_target(m, begin + 0x110, base, size);
    if (!writer || !scheduled || !draw || *writer == *scheduled || *writer == *draw || *scheduled == *draw ||
        !in_module(*writer, writer_code.size(), base, size) ||
        !in_module(*scheduled, scheduled_code.size(), base, size) ||
        !in_module(*draw, draw_code.size(), base, size) ||
        !swgr::code_matches(m, *writer, writer_code, writer_mask) ||
        !swgr::code_matches(m, *scheduled, scheduled_code, scheduled_mask) ||
        !swgr::code_matches(m, *draw, draw_code, draw_mask)) { return false; }
    int32_t first{}, second{};
    if (!m.load(*writer + 6, first) || !m.load(*writer + 17, second)) { return false; }
    const auto low = sdk::discovery::relative_address(*writer + 10, first);
    const auto high = sdk::discovery::relative_address(*writer + 21, second);
    std::array<uint8_t, 32> frame{};
    return low && high && in_module(*low, frame.size(), base, size) && *high == *low + 16 &&
        m.load(*low, frame);
}

struct Packet {
    uintptr_t command_list{}, texture{};
    uint32_t resource_state{}, width{}, height{}, hdr{};
};
static_assert(sizeof(Packet) == 32 && offsetof(Packet, texture) == 8 && offsetof(Packet, hdr) == 28);

struct Target {
    uintptr_t texture{}, resource{}, device{};
    uint64_t generation{};
    swgr::NativeDescription desc{};
};

inline std::optional<Packet> redirected_packet(const Packet& p, uintptr_t command,
    const Target& scene, const Target& ui) {
    using swgr::pointer;
    if (!pointer(command) || p.command_list != command || p.texture != scene.texture || p.resource_state != 4 || p.hdr > 1 ||
        !pointer(scene.texture) || !pointer(ui.texture) || !pointer(scene.resource) || !pointer(ui.resource) ||
        scene.texture == ui.texture || scene.resource == ui.resource || !pointer(scene.device) ||
        scene.device != ui.device || !scene.generation || !ui.generation ||
        scene.desc.width > UINT32_MAX ||
        !swgr::valid_scene(scene.desc, static_cast<uint32_t>(scene.desc.width), scene.desc.height) ||
        !swgr::valid_scene(ui.desc, p.width, p.height) || (ui.desc.format != 87 && ui.desc.format != 90) ||
        (p.hdr == 1 && scene.desc.format != 23 && scene.desc.format != 24)) { return {}; }
    auto redirected = p;
    redirected.texture = ui.texture;
    redirected.hdr = 0; // The dedicated UI is BGRA8; dimensions and normalized movie UVs are unchanged.
    return redirected;
}

// A nested invocation must not inherit the outer callback's command/targets.
template<class T> class Scope {
public:
    Scope(T*& slot, T* value) : m_slot(slot), m_previous(slot) { m_slot = value; }
    ~Scope() { m_slot = m_previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    T*& m_slot;
    T* m_previous;
};
}
