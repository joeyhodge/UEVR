#pragma once

#include "GalacticRacerBink.hpp"
#include "GalacticRacerBinkSeekCode.hpp"

namespace uevr::swgr_bink {
inline constexpr int32_t seek_budget_ms = 6;
inline constexpr size_t seek_boundary = 0xab, tick_boundary = 0x274;
inline constexpr size_t process_entry_size = 32; // The compiler splits this SDK function's unwind ranges.
inline constexpr const char* seek_signature = "41 56 56 57 53 48 81 EC 88 00 00 00 48 8B 05";
inline constexpr const char* tick_signature = "41 57 41 56 56 57 53 48 81 EC 50 01 00 00 44 0F 29";
inline constexpr const char* process_signature = "40 55 41 57 48 83 EC 38 8B EA 44 8B F9 FF 15";

struct SeekFunctions { uintptr_t seek{}, tick{}, process{}; };

template<class Size> inline bool seek_contract(const sdk::discovery::Memory& m,
    const SeekFunctions& f, uintptr_t base, size_t size, Size&& function_size) {
    if (function_size(f.seek) != seek_code.size() || function_size(f.tick) != tick_code.size() ||
        function_size(f.process) != process_entry_size ||
        !in_module(f.seek, seek_code.size(), base, size) || !in_module(f.tick, tick_code.size(), base, size) ||
        !in_module(f.process, process_code.size(), base, size) ||
        !swgr::code_matches(m, f.seek, seek_code, seek_mask) ||
        !swgr::code_matches(m, f.tick, tick_code, tick_mask) ||
        !swgr::code_matches(m, f.process, process_code, process_mask)) { return false; }
    const auto info = call_target(m, f.seek + 0x5d, base, size);
    const auto go = call_target(m, f.seek + seek_boundary, base, size);
    if (!info || !go || *info == *go || function_size(*info) != info_code.size() ||
        function_size(*go) != goto_code.size() || !in_module(*info, info_code.size(), base, size) ||
        !in_module(*go, goto_code.size(), base, size) ||
        !swgr::code_matches(m, *info, info_code, info_mask) ||
        !swgr::code_matches(m, *go, goto_code, goto_mask)) { return false; }
    for (size_t offset : {0xf3, 0x138, 0x17d, 0x1cb, 0x22a, 0x26b, 0x399}) {
        if (call_target(m, f.tick + offset, base, size) != info) { return false; }
    }
    const auto initialize = call_target(m, *info + 0x10, base, size);
    return initialize && call_target(m, *go + 0x17, base, size) == initialize;
}

struct MovieInfo {
    uint64_t buffer_size{}, buffer_used{};
    uint32_t width{}, height{}, frames{}, frame{}, total_frames{}, rate{}, rate_div{}, loops{};
    int32_t read_error{}, texture_error{};
    uint32_t track_type{}, tracks_requested{}, tracks_opened{}, sound_dropouts{}, skipped{}, playback_state{};
    float process_rate{}, alpha{};
};
static_assert(sizeof(MovieInfo) == 0x58 && offsetof(MovieInfo, frame) == 0x1c &&
    offsetof(MovieInfo, playback_state) == 0x4c);
struct Player { uintptr_t handle{}; uint8_t style{}, paused{}, ended{}; };
struct GotoProgress { uint32_t pending{}; int32_t target{}; uint32_t force{}; int32_t budget{}; };
static_assert(sizeof(GotoProgress) == 16);

inline bool overlay_player(const Player& p) {
    return swgr::pointer(p.handle) && p.style >= 1 && p.style <= 4 && !p.paused && !p.ended;
}
inline bool usable_movie(const MovieInfo& i) {
    return i.width && i.width <= 16384 && i.height && i.height <= 16384 &&
        i.frame && i.frame <= i.frames && i.frames <= INT32_MAX && i.rate && i.rate_div &&
        !i.read_error && !i.texture_error;
}
inline bool budget_forward_seek(const Player& p, const MovieInfo& i, uint64_t target, uint64_t budget) {
    return overlay_player(p) && usable_movie(i) && i.playback_state == 0 &&
        target > i.frame && target <= i.frames && static_cast<uint32_t>(budget) == UINT32_MAX;
}
inline bool display_budgeted_seek(const Player& p, const MovieInfo& i, const GotoProgress& g) {
    return overlay_player(p) && usable_movie(i) && i.playback_state == 2 &&
        g.budget == seek_budget_ms && g.pending <= 1 && g.force <= 1 && g.target > 1 &&
        static_cast<uint32_t>(g.target) >= i.frame && static_cast<uint32_t>(g.target) <= i.frames;
}

inline bool load_player(const sdk::discovery::Memory& m, uintptr_t address, Player& p) {
    // Tick's adjusted this (+0x28), Seek and their common Info consumer prove these fields.
    std::array<uint8_t, 0x62> fields{};
    const auto begin = sdk::discovery::relative_address(address, 0xb0);
    if (!swgr::pointer(address) || !begin || !m.load(*begin, fields)) { return false; }
    std::memcpy(&p.handle, fields.data() + 0x58, sizeof(p.handle));
    p.style = fields[0]; p.paused = fields[0x60]; p.ended = fields[0x61];
    return true;
}

inline bool seek_invocation(const sdk::discovery::Memory& m, bool enabled, uintptr_t player,
    uintptr_t handle, uintptr_t saved_handle, uintptr_t stack, uintptr_t info, uint64_t target, uint64_t budget) {
    const auto expected = sdk::discovery::relative_address(stack, 0x20);
    if (!enabled || !swgr::pointer(stack) || !expected || info != *expected || handle != saved_handle) { return false; }
    Player p{}; MovieInfo i{};
    return load_player(m, player, p) && p.handle == handle && m.load(info, i) &&
        budget_forward_seek(p, i, target, budget);
}

inline bool tick_invocation(const sdk::discovery::Memory& m, bool enabled, uintptr_t adjusted_player,
    uintptr_t stack, uintptr_t info) {
    const auto expected = sdk::discovery::relative_address(stack, 0x30);
    const auto player = sdk::discovery::relative_address(adjusted_player, -0x28);
    if (!enabled || !swgr::pointer(stack) || !expected || info != *expected || !player) { return false; }
    MovieInfo i{}; Player p{}; GotoProgress g{};
    if (!m.load(info, i) || i.playback_state != 2 || !load_player(m, *player, p) || !overlay_player(p)) { return false; }
    const auto progress = sdk::discovery::relative_address(p.handle, 0x118);
    // Read only the current caller-owned SDK handle. No saved objects, texture refs or plugin writes.
    return progress && m.load(*progress, g) && display_budgeted_seek(p, i, g);
}

inline uintptr_t overlay_branch_flags(uintptr_t flags, bool display) {
    return display ? flags | uintptr_t{0x40} : flags;
}
}
