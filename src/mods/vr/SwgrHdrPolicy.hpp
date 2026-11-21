#pragma once
// When the SWGR HMD path decodes the scene target from HDR10. No D3D or SDK dependencies.

#include <cstdint>
#include <optional>

namespace swgr_hdr {
constexpr uint32_t format_r10g10b10a2_unorm = 24; // DXGI_FORMAT_R10G10B10A2_UNORM
constexpr uint64_t encoding_recheck_ms = 1000;

// UE's tonemapper writes PQ/BT.2020 only for r.HDR.EnableHDROutput != 0 with
// r.HDR.Display.OutputDevice 3 or 4 (ACES ST2084). Unknown values mean not PQ.
constexpr bool scene_is_pq(std::optional<int32_t> enable_hdr_output, std::optional<int32_t> output_device,
                           uint32_t source_format) {
    return source_format == format_r10g10b10a2_unorm && enable_hdr_output && *enable_hdr_output != 0 &&
        output_device && (*output_device == 3 || *output_device == 4);
}

// Native Fix submits m_game_tex and the separate scene capture as the two eyes; only one would be decoded.
constexpr bool conversion_allowed(bool native_stereo_fix_enabled) { return !native_stereo_fix_enabled; }

// One decision per captured source. A new resource or format re-reads at once; otherwise once per interval.
class EncodingCache {
public:
    template <typename ReadCVars>
    bool is_pq(uintptr_t source, uint32_t format, uint64_t now_ms, ReadCVars&& read) {
        if (!m_valid || source != m_source || format != m_format || now_ms - m_read_ms >= encoding_recheck_ms) {
            const auto [enable, device] = read();
            m_pq = scene_is_pq(enable, device, format);
            m_source = source;
            m_format = format;
            m_read_ms = now_ms;
            m_valid = true;
        }
        return m_pq;
    }
    void reset() { m_valid = false; m_pq = false; }

private:
    uintptr_t m_source{};
    uint32_t m_format{};
    uint64_t m_read_ms{};
    bool m_valid{false};
    bool m_pq{false};
};
} // namespace swgr_hdr
