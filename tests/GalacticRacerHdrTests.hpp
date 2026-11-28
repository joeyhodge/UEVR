#pragma once

#include <optional>
#include <utility>
#include "mods/vr/SwgrHdrPolicy.hpp"

void test_swgr_hdr_policy() {
    namespace h = swgr_hdr;
    constexpr auto r10 = h::format_r10g10b10a2_unorm;
    constexpr uint32_t bgra8 = 87, rgba16f = 10;
    expect(h::scene_is_pq(1, 3, r10) && h::scene_is_pq(1, 4, r10), "HDR output with an ST2084 device decodes PQ");
    expect(!h::scene_is_pq(0, 3, r10), "in-game SDR is never decoded, whatever the desktop mode");
    for (int device : {0, 1, 2, 5, 6, 7, 8, 9}) {
        expect(!h::scene_is_pq(1, device, r10), "only ST2084 output devices are PQ");
    }
    expect(!h::scene_is_pq(1, 3, bgra8) && !h::scene_is_pq(1, 3, rgba16f), "only the R10 scene target can hold PQ");
    expect(!h::scene_is_pq(std::nullopt, 3, r10) && !h::scene_is_pq(1, std::nullopt, r10),
        "an unreadable engine state stays on the plain conversion");
    expect(h::conversion_allowed(false) && !h::conversion_allowed(true),
        "Native Fix keeps both eyes on the plain conversion");

    h::EncodingCache cache;
    int reads = 0;
    std::pair<std::optional<int32_t>, std::optional<int32_t>> state{1, 3};
    const auto read = [&] { ++reads; return state; };
    expect(cache.is_pq(0x1000, r10, 0, read) && reads == 1, "first frame reads the engine state");
    state = {0, 3};
    expect(cache.is_pq(0x1000, r10, h::encoding_recheck_ms - 1, read) && reads == 1,
        "the same source within the interval keeps the decision");
    expect(!cache.is_pq(0x2000, r10, h::encoding_recheck_ms - 1, read) && reads == 2,
        "a new source resource re-reads at once");
    state = {1, 4};
    expect(cache.is_pq(0x2000, r10, 2 * h::encoding_recheck_ms, read) && reads == 3,
        "a game-side HDR toggle is picked up after the interval");
    expect(!cache.is_pq(0x2000, rgba16f, 2 * h::encoding_recheck_ms, read) && reads == 4,
        "a format change re-reads at once");
    cache.reset();
    state = {0, 3};
    expect(!cache.is_pq(0x2000, r10, 2 * h::encoding_recheck_ms, read) && reads == 5, "reset forces a re-read");
}
