#include "DIBRPreview.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <d3dcompiler.h>
#include <spdlog/spdlog.h>

namespace {
constexpr const char* DIBR_PREVIEW_SHADER = R"DIBR(
cbuffer DIBRConstants : register(b0) {
    uint OutputWidth;
    uint OutputHeight;
    uint SourceEyeWidth;
    uint SourceHeight;
    uint DepthWidth;
    uint DepthHeight;
    uint DepthIsDoubleWide;
    uint ReversedDepth;
    float DisparityPixels;
    float EdgeFeather;
    uint UseTrueReprojection;
    float TrueReprojectionStrength;
    float4x4 SourceToRight;
    float LegacyDepthCurve;
    float LegacyNearDepthCap;
    uint EnableDepthEdgeStabilization;
    float DepthEdgeThreshold;
    float DepthEdgeStabilizationStrength;
    uint EnableSpatialRepair;
    uint ShowSpatialRepairMask;
    float RepairResidualPixels;
    float RepairDepthThreshold;
    uint EnableUiEdgeGuard;
    uint ShowUiEdgeGuardMask;
    float UiEdgeGuardStrength;
};

Texture2D<float4> SourceColor : register(t0);
Texture2D<float> SourceDepth : register(t1);
Texture2D<float4> UIAlpha : register(t2);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);

float2 ReprojectSourceUv(float2 source_uv, float raw_depth) {
    const float2 source_ndc = float2(source_uv.x * 2.0f - 1.0f, 1.0f - source_uv.y * 2.0f);
    const float4 target_clip = mul(SourceToRight, float4(source_ndc, raw_depth, 1.0f));
    const float target_w = abs(target_clip.w) > 1e-6f ? target_clip.w : 1e-6f;
    const float2 target_ndc = target_clip.xy / target_w;
    return float2(target_ndc.x * 0.5f + 0.5f, 0.5f - target_ndc.y * 0.5f);
}

float LegacyDepthResponse(float raw_depth) {
    const float near_depth = ReversedDepth != 0 ? raw_depth : 1.0f - raw_depth;
    const float capped_depth = min(saturate(near_depth), max(LegacyNearDepthCap, 1e-4f));
    return pow(capped_depth, max(LegacyDepthCurve, 0.05f));
}

float DepthGradient(float2 uv, float raw_depth) {
    if (DepthWidth == 0 || DepthHeight == 0) {
        return 0.0f;
    }

    const float2 texel = 1.0f / float2((float)DepthWidth, (float)DepthHeight);
    const float x_neighbor = SourceDepth.SampleLevel(LinearClamp, saturate(uv + float2(texel.x, 0.0f)), 0);
    const float y_neighbor = SourceDepth.SampleLevel(LinearClamp, saturate(uv + float2(0.0f, texel.y)), 0);
    return max(abs(raw_depth - x_neighbor), abs(raw_depth - y_neighbor));
}

float2 ResolveSyntheticRightSourceUv(float2 target_uv, float disparity_uv, out uint source_clamped) {
    float2 source_uv = target_uv;
    source_clamped = 0;

    if (UseTrueReprojection != 0) {
        // Match the ordinary DIBR solve exactly. The clamp bit is retained
        // only as confidence information for the optional repair pass.
        [unroll]
        for (uint i = 0; i < 2; ++i) {
            const float source_depth = saturate(SourceDepth.SampleLevel(LinearClamp, source_uv, 0));
            const float2 projected_target = ReprojectSourceUv(source_uv, source_depth);
            const float2 correction = clamp(target_uv - projected_target, -0.125f, 0.125f);
            const float2 candidate = source_uv + correction * TrueReprojectionStrength;
            source_clamped |= (any(candidate < float2(0.0f, 0.0f) || candidate > float2(1.0f, 1.0f)) ? 1u : 0u);
            source_uv = saturate(candidate);
        }
    } else {
        const float desired_u = target_uv.x + disparity_uv;
        source_clamped = desired_u < 0.0f || desired_u > 1.0f ? 1u : 0u;
        source_uv.x = saturate(desired_u);
    }

    return source_uv;
}

bool IsFartherDepth(float candidate, float near_depth, float tolerance) {
    return ReversedDepth != 0 ? candidate + tolerance < near_depth : candidate > near_depth + tolerance;
}

bool IsDepthRunCompatible(float first, float second, float tolerance) {
    return abs(first - second) <= max(tolerance, 0.025f * max(abs(first), abs(second)));
}

uint AnalyzeSpatialRepair(
    float2 target_uv,
    float raw_depth,
    float2 synthesized_source_uv,
    uint source_clamped,
    out float2 repaired_source_uv)
{
    repaired_source_uv = synthesized_source_uv;

    const float source_depth = saturate(SourceDepth.SampleLevel(LinearClamp, synthesized_source_uv, 0));
    const float local_gradient = DepthGradient(target_uv, raw_depth);
    const float depth_tolerance = max(RepairDepthThreshold, local_gradient * 4.0f);
    float residual_pixels = 0.0f;

    if (UseTrueReprojection != 0) {
        const float2 projected_target = ReprojectSourceUv(synthesized_source_uv, source_depth);
        residual_pixels = length((projected_target - target_uv) * float2((float)SourceEyeWidth, (float)OutputHeight));
    }

    const bool low_confidence = source_clamped != 0 ||
        (UseTrueReprojection != 0 && residual_pixels > RepairResidualPixels) ||
        abs(raw_depth - source_depth) > depth_tolerance;
    if (!low_confidence) {
        return 0u;
    }

    // The resolved target-to-source displacement points toward the source-side
    // background for this right-eye synthesis. No displacement means there is
    // no reliable side to search, so retain the ordinary DIBR result.
    const float displacement = synthesized_source_uv.x - target_uv.x;
    const float source_texel = 1.0f / max((float)SourceEyeWidth, 1.0f);
    if (abs(displacement) < source_texel * 0.25f) {
        return 1u;
    }

    const float direction = displacement > 0.0f ? 1.0f : -1.0f;
    const float near_depth = ReversedDepth != 0 ? max(raw_depth, source_depth) : min(raw_depth, source_depth);

    // Require a stable three-sample run. That rejects thin foreground strands
    // and keeps this current-frame repair from copying an isolated occluder.
    [loop]
    for (uint step = 1; step <= 8; ++step) {
        const float2 candidate_uv = synthesized_source_uv + float2(direction * source_texel * (float)step, 0.0f);
        const float2 next_uv = candidate_uv + float2(direction * source_texel, 0.0f);
        const float2 after_next_uv = next_uv + float2(direction * source_texel, 0.0f);
        if (candidate_uv.x < 0.0f || candidate_uv.x > 1.0f ||
            next_uv.x < 0.0f || next_uv.x > 1.0f ||
            after_next_uv.x < 0.0f || after_next_uv.x > 1.0f) {
            break;
        }

        const float candidate_depth = saturate(SourceDepth.SampleLevel(LinearClamp, candidate_uv, 0));
        const float next_depth = saturate(SourceDepth.SampleLevel(LinearClamp, next_uv, 0));
        const float after_next_depth = saturate(SourceDepth.SampleLevel(LinearClamp, after_next_uv, 0));
        if (!IsFartherDepth(candidate_depth, near_depth, depth_tolerance) ||
            !IsDepthRunCompatible(candidate_depth, next_depth, depth_tolerance) ||
            !IsDepthRunCompatible(candidate_depth, after_next_depth, depth_tolerance)) {
            continue;
        }

        // Pick the farthest point inside the first stable background run, but
        // never jump beyond it into unrelated distant geometry.
        repaired_source_uv = candidate_uv;
        float selected_depth = candidate_depth;
        if (IsFartherDepth(next_depth, selected_depth, depth_tolerance * 0.25f)) {
            repaired_source_uv = next_uv;
            selected_depth = next_depth;
        }
        if (IsFartherDepth(after_next_depth, selected_depth, depth_tolerance * 0.25f)) {
            repaired_source_uv = after_next_uv;
        }
        return 2u;
    }

    return 1u;
}

float4 ApplySpatialRepairMask(float4 color, uint repair_state) {
    if (repair_state == 2u) {
        color.rgb = lerp(color.rgb, float3(1.0f, 0.10f, 0.02f), 0.65f);
    } else if (repair_state == 1u) {
        color.rgb = lerp(color.rgb, float3(1.0f, 0.62f, 0.02f), 0.45f);
    }

    return color;
}

float UIAlphaEdgeMask(float2 uv) {
    if (EnableUiEdgeGuard == 0) {
        return 0.0f;
    }

    uint width = 0;
    uint height = 0;
    UIAlpha.GetDimensions(width, height);
    if (width == 0 || height == 0) {
        return 0.0f;
    }

    const int2 max_coord = int2((int)width - 1, (int)height - 1);
    const int2 center = clamp(
        int2(uv * float2((float)width, (float)height)),
        int2(0, 0),
        max_coord);

    // A two-pixel Manhattan dilation is deliberately sparse: nine alpha
    // loads instead of a 5x5 filter. It still catches horizontal, vertical,
    // and antialiased UI edges while keeping this optional compute pass light.
    const float center_alpha = saturate(UIAlpha.Load(int3(center, 0)).a);
    float min_alpha = center_alpha;
    float max_alpha = center_alpha;
    [unroll]
    for (int step = 1; step <= 2; ++step) {
        const int2 horizontal = int2(step, 0);
        const int2 vertical = int2(0, step);
        const float left = saturate(UIAlpha.Load(int3(clamp(center - horizontal, int2(0, 0), max_coord), 0)).a);
        const float right = saturate(UIAlpha.Load(int3(clamp(center + horizontal, int2(0, 0), max_coord), 0)).a);
        const float up = saturate(UIAlpha.Load(int3(clamp(center - vertical, int2(0, 0), max_coord), 0)).a);
        const float down = saturate(UIAlpha.Load(int3(clamp(center + vertical, int2(0, 0), max_coord), 0)).a);
        min_alpha = min(min_alpha, min(min(left, right), min(up, down)));
        max_alpha = max(max_alpha, max(max(left, right), max(up, down)));
    }

    const float has_ui = smoothstep(0.01f, 0.12f, max_alpha);
    const float is_not_ui_interior = 1.0f - smoothstep(0.98f, 0.999f, min_alpha);
    const float transition = smoothstep(0.005f, 0.12f, max_alpha - min_alpha);
    return saturate(has_ui * is_not_ui_interior * transition);
}

float4 ApplyUIAlphaEdgeMask(float4 color, float edge_mask) {
    color.rgb = lerp(color.rgb, float3(0.05f, 0.90f, 1.0f), 0.60f * edge_mask);
    return color;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID) {
    if (dispatch_id.x >= OutputWidth || dispatch_id.y >= OutputHeight || SourceEyeWidth == 0) {
        return;
    }

    const bool target_right = dispatch_id.x >= SourceEyeWidth;
    const float target_u = (float)(dispatch_id.x - (target_right ? SourceEyeWidth : 0) + 0.5) / (float)SourceEyeWidth;
    const float v = (float)(dispatch_id.y + 0.5) / (float)OutputHeight;
    const float2 target_uv = float2(target_u, v);
    const float raw_depth = saturate(SourceDepth.SampleLevel(LinearClamp, target_uv, 0));
    const float edge = smoothstep(0.0, EdgeFeather, target_u) * (1.0 - smoothstep(1.0 - EdgeFeather, 1.0, target_u));
    const float disparity_uv = (DisparityPixels * LegacyDepthResponse(raw_depth) * edge) / (float)SourceEyeWidth;

    uint source_clamped = 0u;
    float2 synthesized_source_uv = target_uv;
    // Keep the ordinary left-eye path untouched unless the optional binocular
    // diagnostic overlay needs the matching right-eye classification.
    if (target_right || (EnableSpatialRepair != 0 && ShowSpatialRepairMask != 0)) {
        synthesized_source_uv = ResolveSyntheticRightSourceUv(target_uv, disparity_uv, source_clamped);
    }
    float2 source_uv = target_right ? synthesized_source_uv : target_uv;

    if (target_right && EnableDepthEdgeStabilization != 0) {
        // A single left-eye image cannot reveal data behind a foreground edge.
        // Pull only high-confidence discontinuities back toward the stable left
        // sample instead of allowing foreground color to smear across a hole.
        const float source_depth = saturate(SourceDepth.SampleLevel(LinearClamp, source_uv, 0));
        const float discontinuity = max(DepthGradient(target_uv, raw_depth), abs(raw_depth - source_depth));
        const float threshold = max(DepthEdgeThreshold, 1e-5f);
        const float stabilization = smoothstep(threshold, threshold * 4.0f, discontinuity) *
            saturate(DepthEdgeStabilizationStrength);
        source_uv = lerp(source_uv, target_uv, stabilization);
    }

    uint repair_state = 0u;
    float2 repaired_source_uv = synthesized_source_uv;
    // The debug overlay mirrors the right-eye analysis into the left eye so it
    // stays comfortable to inspect without creating a binocular mismatch.
    if (EnableSpatialRepair != 0 && (target_right || ShowSpatialRepairMask != 0)) {
        repair_state = AnalyzeSpatialRepair(
            target_uv,
            raw_depth,
            synthesized_source_uv,
            source_clamped,
            repaired_source_uv);
    }

    if (target_right && repair_state == 2u) {
        source_uv = repaired_source_uv;
    }

    float4 output_color = SourceColor.SampleLevel(LinearClamp, source_uv, 0);
    if (EnableSpatialRepair != 0 && ShowSpatialRepairMask != 0) {
        output_color = ApplySpatialRepairMask(output_color, repair_state);
    }

    // The normal pass computes the mask only for the synthesized eye. The
    // debug overlay additionally mirrors it into the left eye for comfort.
    const float ui_edge_mask = (target_right || ShowUiEdgeGuardMask != 0) ? UIAlphaEdgeMask(target_uv) : 0.0f;
    if (target_right && ui_edge_mask > 0.0f) {
        // Hold only the synthesized scene under the alpha transition against
        // the stable left-eye source. The real UI RGBA is submitted separately
        // by OpenXR and remains completely untouched.
        const float3 stable_left_scene = SourceColor.SampleLevel(LinearClamp, target_uv, 0).rgb;
        output_color.rgb = lerp(
            output_color.rgb,
            stable_left_scene,
            saturate(UiEdgeGuardStrength) * ui_edge_mask);
    }

    // Mirror the diagnostic overlay into both eyes so it is comfortable to
    // inspect without creating a binocular mismatch.
    if (ShowUiEdgeGuardMask != 0) {
        output_color = ApplyUIAlphaEdgeMask(output_color, ui_edge_mask);
    }

    Output[dispatch_id.xy] = output_color;
}
)DIBR";

std::string shader_error_text(ID3DBlob* errors, HRESULT hr) {
    if (errors != nullptr && errors->GetBufferPointer() != nullptr && errors->GetBufferSize() > 0) {
        return std::string{static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()};
    }

    char buffer[32]{};
    sprintf_s(buffer, "HRESULT 0x%08X", static_cast<uint32_t>(hr));
    return buffer;
}

bool valid_2d_non_msaa_desc(const D3D12_RESOURCE_DESC& desc) {
    return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc.Width > 0 &&
        desc.Height > 0 &&
        desc.Width <= 32768 &&
        desc.Height <= 32768 &&
        desc.SampleDesc.Count == 1 &&
        desc.DepthOrArraySize == 1;
}

uint32_t dsv_array_slice(const D3D12_DEPTH_STENCIL_VIEW_DESC* desc) {
    if (desc == nullptr) {
        return 0;
    }

    switch (desc->ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
        return desc->Texture2DArray.FirstArraySlice;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return desc->Texture2DMSArray.FirstArraySlice;
    default:
        return 0;
    }
}

constexpr auto CAPTURED_DEPTH_SHADER_READ =
    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
} // namespace

namespace d3d12 {
DIBRPreview::~DIBRPreview() {
    reset();
}

const char* DIBRPreview::status_name() const {
    switch (m_status.load(std::memory_order_acquire)) {
    case Status::Idle: return "idle";
    case Status::Waiting: return "waiting for a compatible scene/depth pair";
    case Status::Ready: return "ready";
    case Status::Failed: return "blocked";
    }

    return "unknown";
}

std::string DIBRPreview::failure_reason() const {
    std::scoped_lock _{m_status_mutex};
    return m_failure_reason;
}

std::string DIBRPreview::depth_trace_summary() const {
    std::scoped_lock _{m_status_mutex};
    return m_depth_trace_summary;
}

void DIBRPreview::set_depth_trace_expected_extent(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) {
        return;
    }

    bool changed{};
    {
        std::scoped_lock _{m_status_mutex};
        if (m_depth_trace_expected_width == width && m_depth_trace_expected_height == height) {
            return;
        }

        m_depth_trace_expected_width = width;
        m_depth_trace_expected_height = height;
        refresh_depth_trace_candidate_locked();
        changed = true;
    }

    if (changed) {
        // A resize invalidates both the selected DSV shape and the persistent
        // copy. Do not synthesize from a previous map's depth after a resize.
        std::scoped_lock _{m_capture_mutex};
        m_captured_depth.Reset();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
        m_ue5_rdg_depth_capture_requested.store(
            m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire),
            std::memory_order_release);
        SPDLOG_INFO("[DIBR][RDG trace] presentation source is {}x{}; accepting matching full-size or per-eye DSV depth", width, height);
    }
}

void DIBRPreview::set_ue5_rdg_depth_capture_enabled(bool enabled) {
    const auto previous = m_ue5_rdg_depth_capture_enabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled) {
        return;
    }

    if (!enabled) {
        m_ue5_rdg_depth_capture_requested.store(false, std::memory_order_release);
        std::scoped_lock _{m_capture_mutex};
        m_captured_depth.Reset();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
        SPDLOG_INFO("[DIBR][RDG capture] disabled; released the owned depth copy");
        return;
    }

    m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
    SPDLOG_INFO("[DIBR][RDG capture] enabled; pacing capture to one verified depth copy per DIBR frame");
}

void DIBRPreview::request_ue5_rdg_depth_capture() {
    if (m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire)) {
        m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
    }
}

Microsoft::WRL::ComPtr<ID3D12Resource> DIBRPreview::captured_depth_snapshot() const {
    std::scoped_lock _{m_capture_mutex};
    return m_captured_depth;
}

bool DIBRPreview::has_captured_depth() const {
    std::scoped_lock _{m_capture_mutex};
    return m_captured_depth != nullptr;
}

void DIBRPreview::refresh_depth_trace_candidate_locked() {
    uintptr_t selected{};
    const DepthTraceCandidate* selected_candidate{};
    enum class MatchKind : uint8_t {
        None,
        Fallback,
        Eye,
        Full,
    } match_kind{MatchKind::None};
    uint64_t fallback_area{};
    const auto expected_eye_width = m_depth_trace_expected_width / 2;

    for (const auto& [resource, candidate] : m_traced_depth_resources) {
        if (candidate.sample_count != 1 ||
            depth_srv_format(candidate.resource_format) == DXGI_FORMAT_UNKNOWN) {
            continue;
        }

        if (candidate.width == m_depth_trace_expected_width && candidate.height == m_depth_trace_expected_height) {
            selected = resource;
            selected_candidate = &candidate;
            match_kind = MatchKind::Full;
            break;
        }

        // UE5.4+ RDG commonly keeps SceneDepthZ as a two-slice texture array:
        // the scene colour is packed stereo, but each DSV is one eye wide.
        // That is the intended source for DIBR, not an arbitrary fallback.
        if (expected_eye_width != 0 && candidate.width == expected_eye_width &&
            candidate.height == m_depth_trace_expected_height && candidate.array_size >= 1) {
            if (match_kind != MatchKind::Eye || candidate.array_size > selected_candidate->array_size) {
                selected = resource;
                selected_candidate = &candidate;
                match_kind = MatchKind::Eye;
            }
            continue;
        }

        const auto area = candidate.width * candidate.height;
        if (match_kind == MatchKind::None && area > fallback_area) {
            fallback_area = area;
            selected = resource;
            selected_candidate = &candidate;
            match_kind = MatchKind::Fallback;
        }
    }

    const auto previous = m_depth_trace_candidate.exchange(selected, std::memory_order_acq_rel);
    if (selected == 0) {
        m_depth_trace_summary = "no exact DSV match for scene " +
            std::to_string(m_depth_trace_expected_width) + "x" +
            std::to_string(m_depth_trace_expected_height);
        return;
    }

    if (selected != previous && selected_candidate != nullptr) {
        m_depth_trace_candidate_array_slice.store(selected_candidate->array_slice, std::memory_order_release);
        m_ue5_rdg_depth_capture_requested.store(
            m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire),
            std::memory_order_release);
        std::ostringstream summary{};
        summary << "selected DSV 0x" << std::hex << std::uppercase << selected << std::dec
                << " for scene " << m_depth_trace_expected_width << "x" << m_depth_trace_expected_height
                << (match_kind == MatchKind::Full ? " (full-size exact)" :
                    match_kind == MatchKind::Eye ? " (per-eye exact)" :
                    " (largest fallback; not adoptable yet)")
                << " candidate=" << selected_candidate->width << "x" << selected_candidate->height
                << " format=" << static_cast<uint32_t>(selected_candidate->resource_format)
                << " view_format=" << static_cast<uint32_t>(selected_candidate->view_format)
                << " array=" << selected_candidate->array_size
                << " slice=" << selected_candidate->array_slice
                << "; waiting for final resource state";
        m_depth_trace_summary = summary.str();
        m_depth_trace_last_state.store(D3D12_RESOURCE_STATE_COMMON, std::memory_order_release);
        SPDLOG_INFO("[DIBR][RDG trace] {}", m_depth_trace_summary);
    }
}

bool DIBRPreview::is_trace_candidate_compatible_locked(uintptr_t resource) const {
    const auto it = m_traced_depth_resources.find(resource);
    if (it == m_traced_depth_resources.end() ||
        it->second.sample_count != 1 ||
        depth_srv_format(it->second.resource_format) == DXGI_FORMAT_UNKNOWN ||
        m_depth_trace_expected_width == 0 ||
        m_depth_trace_expected_height == 0) {
        return false;
    }

    const auto expected_eye_width = m_depth_trace_expected_width / 2;
    return it->second.height == m_depth_trace_expected_height &&
        (it->second.width == m_depth_trace_expected_width ||
         (expected_eye_width != 0 && it->second.width == expected_eye_width));
}

void DIBRPreview::on_depth_stencil_view_created(
    ID3D12Resource* resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor)
{
    if (resource == nullptr || descriptor.ptr == 0) {
        return;
    }

    const auto resource_desc = resource->GetDesc();
    if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        resource_desc.Width == 0 || resource_desc.Height == 0) {
        return;
    }

    const auto resource_key = reinterpret_cast<uintptr_t>(resource);
    std::scoped_lock _{m_status_mutex};
    const auto view_slice = dsv_array_slice(desc);
    if (const auto existing = m_traced_depth_resources.find(resource_key); existing != m_traced_depth_resources.end()) {
        existing->second.array_slice = (std::min)(existing->second.array_slice, view_slice);
        if (m_depth_trace_candidate.load(std::memory_order_acquire) == resource_key) {
            m_depth_trace_candidate_array_slice.store(existing->second.array_slice, std::memory_order_release);
        }
        return;
    }

    m_traced_depth_resources.emplace(
        resource_key,
        DepthTraceCandidate{
            .resource = resource_key,
            .width = resource_desc.Width,
            .height = resource_desc.Height,
            .resource_format = resource_desc.Format,
            .flags = resource_desc.Flags,
            .sample_count = resource_desc.SampleDesc.Count,
            .view_format = desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN,
            .array_size = resource_desc.DepthOrArraySize,
            .array_slice = view_slice,
        });
    refresh_depth_trace_candidate_locked();

    // The DSV hook sees every shadow, reflection and scene-capture target.
    // Only announce candidates that could actually match the presentation
    // source; this keeps a moving camera from turning the diagnostic into a
    // stream of unrelated texture sizes.
    if (!is_trace_candidate_compatible_locked(resource_key)) {
        return;
    }

    std::ostringstream summary{};
    summary << "DSV 0x" << std::hex << std::uppercase << resource_key << std::dec
            << " " << resource_desc.Width << "x" << resource_desc.Height
            << " format=" << static_cast<uint32_t>(resource_desc.Format)
            << " flags=0x" << std::hex << std::uppercase << resource_desc.Flags << std::dec
            << " samples=" << resource_desc.SampleDesc.Count
            << " array=" << resource_desc.DepthOrArraySize;
    if (desc != nullptr) {
        summary << " view_format=" << static_cast<uint32_t>(desc->Format)
                << " view_dimension=" << static_cast<uint32_t>(desc->ViewDimension)
                << " slice=" << view_slice;
    }

    const auto trace = summary.str();
    m_depth_trace_summary = trace;
    SPDLOG_INFO("[DIBR][RDG trace] {} handle=0x{:X}", trace, descriptor.ptr);
}

bool DIBRPreview::ensure_captured_depth_locked(
    ID3D12Device* device,
    const D3D12_RESOURCE_DESC& source_desc,
    std::string& reason)
{
    if (device == nullptr || source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        source_desc.Width == 0 || source_desc.Height == 0 ||
        source_desc.Width > 32768 || source_desc.Height > 32768 ||
        source_desc.SampleDesc.Count != 1 || source_desc.DepthOrArraySize == 0 ||
        source_desc.MipLevels != 1 || depth_srv_format(source_desc.Format) == DXGI_FORMAT_UNKNOWN) {
        reason = "selected DSV resource is not a single-mip non-MSAA shader-readable depth texture";
        return false;
    }

    if (m_captured_depth != nullptr &&
        m_captured_depth_width == source_desc.Width &&
        m_captured_depth_height == source_desc.Height &&
        m_captured_depth_format == source_desc.Format) {
        return true;
    }

    m_captured_depth.Reset();
    m_captured_depth_width = 0;
    m_captured_depth_height = 0;
    m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
    m_captured_depth_generation = 0;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    // CopyResource only needs a compatible typeless texture. Removing the
    // depth-stencil flag lets the persistent copy expose an SRV later, while
    // the game-owned source keeps its original DSV and state untouched.
    auto destination_desc = source_desc;
    // The source may be a two-eye texture array. DIBR needs one selected eye
    // as a normal Texture2D, so its persistent copy always owns one slice.
    destination_desc.DepthOrArraySize = 1;
    destination_desc.Flags &= ~(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    destination_desc.Alignment = 0;

    ComPtr captured{};
    const auto create_result = device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &destination_desc,
            CAPTURED_DEPTH_SHADER_READ,
            nullptr,
            IID_PPV_ARGS(&captured));
    if (FAILED(create_result)) {
        char text[64]{};
        sprintf_s(text, "CreateCommittedResource failed 0x%08X", static_cast<uint32_t>(create_result));
        reason = text;
        return false;
    }

    captured->SetName(L"DIBR UE5 RDG Depth Copy");
    m_captured_depth = std::move(captured);
    m_captured_depth_width = source_desc.Width;
    m_captured_depth_height = source_desc.Height;
    m_captured_depth_format = source_desc.Format;
    return true;
}

bool DIBRPreview::capture_depth_before_restore_locked(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* source,
    D3D12_RESOURCE_STATES source_state,
    uint32_t source_array_slice,
    UINT source_transition_subresource)
{
    if (command_list == nullptr || source == nullptr ||
        source_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        m_capture_failure_reason = "capture callback did not receive the expected NPSR command-list state";
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    const auto device_result = source->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(device_result) || device == nullptr) {
        char text[64]{};
        sprintf_s(text, "ID3D12Resource::GetDevice failed 0x%08X", static_cast<uint32_t>(device_result));
        m_capture_failure_reason = text;
        return false;
    }

    const auto source_desc = source->GetDesc();
    if (source_array_slice >= source_desc.DepthOrArraySize) {
        m_capture_failure_reason = "selected DSV array slice is outside the source resource";
        return false;
    }

    // The RDG depth capture only supports plane zero and one mip. Keep the
    // D3D12 subresource calculation explicit so this code does not depend on
    // D3DX helper headers being available in the injected build.
    const auto source_subresource = static_cast<UINT>(source_array_slice * source_desc.MipLevels);
    if (source_transition_subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
        source_transition_subresource != source_subresource)
    {
        m_capture_failure_reason = "selected DSV slice did not match the engine transition subresource";
        return false;
    }

    std::string allocation_reason{};
    if (!ensure_captured_depth_locked(device.Get(), source_desc, allocation_reason)) {
        m_capture_failure_reason = std::move(allocation_reason);
        return false;
    }

    // This is recorded immediately before the engine's NPSR -> DEPTH_WRITE
    // transition. UE5.7 can transition one array slice at a time, so preserve
    // the original subresource exactly instead of changing both eyes' state.
    // The source state is restored before the original barrier runs.
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    }
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.Subresource = source_transition_subresource;
    barriers[0].Transition.StateBefore = source_state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = m_captured_depth.Get();
    barriers[1].Transition.Subresource = 0;
    barriers[1].Transition.StateBefore = CAPTURED_DEPTH_SHADER_READ;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = source;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = source_subresource;
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = m_captured_depth.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, nullptr);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = source_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = CAPTURED_DEPTH_SHADER_READ;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    ++m_captured_depth_generation;
    return true;
}

void DIBRPreview::on_resource_barriers(
    ID3D12GraphicsCommandList* command_list,
    UINT count,
    const D3D12_RESOURCE_BARRIER* barriers)
{
    if (barriers == nullptr) {
        return;
    }

    // The DSV selection happens when its descriptor is created. ResourceBarrier
    // is a renderer hot path, so do not take the diagnostic map lock for every
    // unrelated transition. A selected candidate is already compatibility-checked.
    const auto candidate = m_depth_trace_candidate.load(std::memory_order_acquire);
    if (candidate == 0) {
        return;
    }

    const auto capture_enabled = m_ue5_rdg_depth_capture_enabled.load(std::memory_order_acquire);
    const auto candidate_slice = m_depth_trace_candidate_array_slice.load(std::memory_order_acquire);

    for (UINT i = 0; i < count; ++i) {
        const auto& barrier = barriers[i];
        if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            continue;
        }

        const auto resource_key = reinterpret_cast<uintptr_t>(barrier.Transition.pResource);
        if (resource_key != candidate) {
            continue;
        }

        const auto before = barrier.Transition.StateBefore;
        const auto after = static_cast<uint32_t>(barrier.Transition.StateAfter);
        const auto previous = m_depth_trace_last_state.exchange(after, std::memory_order_acq_rel);
        const auto closing_capture_window =
            before == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE &&
            barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_DEPTH_WRITE;

        if (capture_enabled && closing_capture_window) {
            const auto source_desc = barrier.Transition.pResource->GetDesc();
            if (candidate_slice >= source_desc.DepthOrArraySize) {
                continue;
            }

            const auto selected_subresource = static_cast<UINT>(candidate_slice * source_desc.MipLevels);
            if (barrier.Transition.Subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                barrier.Transition.Subresource != selected_subresource)
            {
                // The selected eye was not the one the engine is about to
                // overwrite. Leave its state entirely to the game.
                continue;
            }

            // UE5.7 can cycle the same RDG depth resource through several
            // shader-read/write windows before Present. Each capture is a full
            // depth copy plus four barriers, so keep only the next window the
            // DIBR consumer explicitly requested.
            if (!m_ue5_rdg_depth_capture_requested.exchange(false, std::memory_order_acq_rel)) {
                continue;
            }

            bool captured{};
            uint64_t generation{};
            bool log_success{};
            bool log_failure{};
            std::string capture_failure{};
            {
                std::scoped_lock _{m_capture_mutex};
                captured = capture_depth_before_restore_locked(
                    command_list,
                    barrier.Transition.pResource,
                    before,
                    candidate_slice,
                    barrier.Transition.Subresource);
                generation = m_captured_depth_generation;
                if (captured && !m_capture_success_logged) {
                    m_capture_success_logged = true;
                    log_success = true;
                } else if (!captured && !m_capture_failure_logged) {
                    m_capture_failure_logged = true;
                    log_failure = true;
                    capture_failure = m_capture_failure_reason;
                }
            }

            if (captured) {
                std::ostringstream summary{};
                summary << "captured verified RDG depth 0x" << std::hex << std::uppercase << candidate << std::dec
                        << " generation=" << generation
                        << " during NPSR -> DEPTH_WRITE; source state restored before the original barrier";
                {
                    std::scoped_lock _{m_status_mutex};
                    m_depth_trace_summary = summary.str();
                }
                if (log_success) {
                    SPDLOG_INFO("[DIBR][RDG capture] {}", summary.str());
                }
            } else if (log_failure) {
                SPDLOG_WARN(
                    "[DIBR][RDG capture] verified depth window could not be copied; reason={}; falling back to the normal scene path",
                    capture_failure);
            }

            // Do not permanently lose the first capture because a resource
            // resize or transient command-list failure made this window
            // unsuitable. A later verified transition may still be valid.
            if (!captured) {
                m_ue5_rdg_depth_capture_requested.store(true, std::memory_order_release);
            }
            continue;
        }

        if (previous == after || capture_enabled) {
            continue;
        }

        const auto shader_readable =
            (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) != 0 ||
            (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        std::ostringstream summary{};
        summary << "selected DSV 0x" << std::hex << std::uppercase << candidate << std::dec
                << " transition 0x" << std::hex << std::uppercase << barrier.Transition.StateBefore
                << " -> 0x" << barrier.Transition.StateAfter << std::dec
                << (shader_readable ? " (shader-readable; not yet adopted)" : " (not shader-readable)");
        {
            std::scoped_lock _{m_status_mutex};
            m_depth_trace_summary = summary.str();
        }
        SPDLOG_INFO("[DIBR][RDG trace] {}", summary.str());
    }
}

void DIBRPreview::fail(std::string reason) {
    std::scoped_lock _{m_status_mutex};
    if (m_status.load(std::memory_order_acquire) != Status::Failed || m_failure_reason != reason) {
        SPDLOG_WARN("[DIBR] Disabled: {}", reason);
    }

    m_failure_reason = std::move(reason);
    m_status.store(Status::Failed, std::memory_order_release);
}

void DIBRPreview::reset() {
    if (m_constants != nullptr && m_constants_cpu != nullptr) {
        m_constants->Unmap(0, nullptr);
    }

    m_output.reset();
    m_source_staging.Reset();
    m_source_staging_width = 0;
    m_source_staging_height = 0;
    m_source_staging_format = DXGI_FORMAT_UNKNOWN;
    m_constants_cpu = nullptr;
    m_constants.Reset();
    m_descriptor_heap.Reset();
    m_pipeline_state.Reset();
    m_root_signature.Reset();
    m_output_width = 0;
    m_output_height = 0;
    m_output_format = DXGI_FORMAT_UNKNOWN;
    m_descriptor_increment = 0;
    m_output_has_been_written = false;
    {
        std::scoped_lock _{m_capture_mutex};
        m_captured_depth.Reset();
        m_captured_depth_width = 0;
        m_captured_depth_height = 0;
        m_captured_depth_format = DXGI_FORMAT_UNKNOWN;
        m_captured_depth_generation = 0;
        m_capture_success_logged = false;
        m_capture_failure_logged = false;
        m_capture_failure_reason.clear();
    }
    {
        std::scoped_lock _{m_status_mutex};
        m_failure_reason.clear();
        m_traced_depth_resources.clear();
        m_depth_trace_summary = "waiting for a DSV candidate";
        m_depth_trace_expected_width = 0;
        m_depth_trace_expected_height = 0;
    }
    m_depth_trace_candidate.store(0, std::memory_order_release);
    m_depth_trace_candidate_array_slice.store(0, std::memory_order_release);
    m_depth_trace_last_state.store(D3D12_RESOURCE_STATE_COMMON, std::memory_order_release);
    m_ue5_rdg_depth_capture_enabled.store(false, std::memory_order_release);
    m_ue5_rdg_depth_capture_requested.store(false, std::memory_order_release);
    m_status.store(Status::Idle, std::memory_order_release);
}

DXGI_FORMAT DIBRPreview::color_srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        // UE5 HDR scene targets are commonly one of these formats. The
        // compute shader samples them as float4 and converts only the
        // synthesized preview output to the existing LDR presentation path.
        return format;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT DIBRPreview::depth_srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool DIBRPreview::validate_inputs(ID3D12Resource* color, ID3D12Resource* depth, std::string& reason) const {
    if (color == nullptr || depth == nullptr) {
        reason = "scene color or SceneDepthZ is unavailable";
        return false;
    }

    const auto color_desc = color->GetDesc();
    const auto depth_desc = depth->GetDesc();

    if (!valid_2d_non_msaa_desc(color_desc) || (color_desc.Width % 2) != 0) {
        reason = "scene color is not an even-width non-MSAA 2D texture";
        return false;
    }

    if (!valid_2d_non_msaa_desc(depth_desc)) {
        reason = "SceneDepthZ is not a non-MSAA 2D texture";
        return false;
    }

    if (color_srv_format(color_desc.Format) == DXGI_FORMAT_UNKNOWN) {
        reason = "scene color format " + std::to_string(static_cast<uint32_t>(color_desc.Format)) +
            " is not supported by the DIBR preview";
        return false;
    }

    if ((color_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        reason = "scene color denies shader-resource views";
        return false;
    }

    if (depth_srv_format(depth_desc.Format) == DXGI_FORMAT_UNKNOWN) {
        reason = "SceneDepthZ format is not shader-readable by the DIBR preview";
        return false;
    }

    if ((depth_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
        reason = "SceneDepthZ denies shader-resource views";
        return false;
    }

    if (depth_desc.Height != color_desc.Height ||
        (depth_desc.Width != color_desc.Width && depth_desc.Width != color_desc.Width / 2)) {
        reason = "SceneDepthZ extent does not match the scene eye extent";
        return false;
    }

    return true;
}

bool DIBRPreview::ensure_pipeline(ID3D12Device* device) {
    if (m_root_signature != nullptr && m_pipeline_state != nullptr &&
        m_descriptor_heap != nullptr && m_constants != nullptr && m_constants_cpu != nullptr) {
        m_status.store(Status::Ready, std::memory_order_release);
        return true;
    }

    if (m_status.load(std::memory_order_acquire) == Status::Failed || device == nullptr) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3DBlob> shader{};
    Microsoft::WRL::ComPtr<ID3DBlob> errors{};
    const auto compile_result = D3DCompile(
        DIBR_PREVIEW_SHADER,
        strlen(DIBR_PREVIEW_SHADER),
        "UEVR_DIBRPreview",
        nullptr,
        nullptr,
        "main",
        "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        &shader,
        &errors);

    if (FAILED(compile_result) || shader == nullptr) {
        fail("DIBR compute shader compilation failed: " + shader_error_text(errors.Get(), compile_result));
        return false;
    }

    D3D12_DESCRIPTOR_RANGE descriptor_ranges[2]{};
    descriptor_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    descriptor_ranges[0].NumDescriptors = 3;
    descriptor_ranges[0].BaseShaderRegister = 0;
    descriptor_ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    descriptor_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    descriptor_ranges[1].NumDescriptors = 1;
    descriptor_ranges[1].BaseShaderRegister = 0;
    descriptor_ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER root_parameters[3]{};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[0].DescriptorTable.pDescriptorRanges = &descriptor_ranges[0];
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
    root_parameters[1].DescriptorTable.pDescriptorRanges = &descriptor_ranges[1];
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    root_parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    root_parameters[2].Descriptor.ShaderRegister = 0;
    root_parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
    root_signature_desc.NumParameters = static_cast<UINT>(std::size(root_parameters));
    root_signature_desc.pParameters = root_parameters;
    root_signature_desc.NumStaticSamplers = 1;
    root_signature_desc.pStaticSamplers = &sampler;
    root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> serialized{};
    const auto serialize_result = D3D12SerializeRootSignature(
        &root_signature_desc,
        D3D_ROOT_SIGNATURE_VERSION_1,
        &serialized,
        &errors);
    if (FAILED(serialize_result) || serialized == nullptr) {
        fail("DIBR root signature serialization failed: " + shader_error_text(errors.Get(), serialize_result));
        return false;
    }

    if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&m_root_signature)))) {
        fail("DIBR root signature creation failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = m_root_signature.Get();
    pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
    if (FAILED(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&m_pipeline_state)))) {
        fail("DIBR compute pipeline creation failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 4;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&m_descriptor_heap)))) {
        fail("DIBR descriptor heap creation failed");
        return false;
    }

    m_descriptor_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC constants_desc{};
    constants_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    constants_desc.Width = sizeof(Constants);
    constants_desc.Height = 1;
    constants_desc.DepthOrArraySize = 1;
    constants_desc.MipLevels = 1;
    constants_desc.SampleDesc.Count = 1;
    constants_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &constants_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_constants)))) {
        fail("DIBR constants upload allocation failed");
        return false;
    }

    if (FAILED(m_constants->Map(0, nullptr, reinterpret_cast<void**>(&m_constants_cpu))) || m_constants_cpu == nullptr) {
        fail("DIBR constants upload mapping failed");
        return false;
    }

    {
        std::scoped_lock _{m_status_mutex};
        m_failure_reason.clear();
    }
    m_status.store(Status::Ready, std::memory_order_release);
    SPDLOG_INFO("[DIBR] Preview compute pipeline initialized. It is isolated from native fix, synced, AFR, UI, and spectator paths.");
    return true;
}

bool DIBRPreview::ensure_output(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    if (m_output.texture != nullptr && m_output_width == width && m_output_height == height && m_output_format == format) {
        return true;
    }

    m_output.reset();
    m_output_width = 0;
    m_output_height = 0;
    m_output_format = DXGI_FORMAT_UNKNOWN;
    m_output_has_been_written = false;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC output_desc{};
    output_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    output_desc.Width = width;
    output_desc.Height = height;
    output_desc.DepthOrArraySize = 1;
    output_desc.MipLevels = 1;
    output_desc.Format = format;
    output_desc.SampleDesc.Count = 1;
    output_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    Microsoft::WRL::ComPtr<ID3D12Resource> output{};
    if (FAILED(device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &output_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&output)))) {
        fail("DIBR output texture allocation failed");
        return false;
    }

    if (!m_output.setup(device, output.Get(), format, format, L"DIBR Preview Output")) {
        fail("DIBR output texture view creation failed");
        return false;
    }

    m_output_width = width;
    m_output_height = height;
    m_output_format = format;
    SPDLOG_INFO("[DIBR] Created packed output {}x{} format={} for direct presentation copy", width, height, static_cast<uint32_t>(format));
    return true;
}

bool DIBRPreview::ensure_source_staging(ID3D12Device* device, const D3D12_RESOURCE_DESC& color_desc) {
    const auto eye_width = static_cast<uint32_t>(color_desc.Width / 2);
    if (m_source_staging != nullptr &&
        m_source_staging_width == eye_width &&
        m_source_staging_height == color_desc.Height &&
        m_source_staging_format == color_desc.Format) {
        return true;
    }

    m_source_staging.Reset();
    m_source_staging_width = 0;
    m_source_staging_height = 0;
    m_source_staging_format = DXGI_FORMAT_UNKNOWN;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Preserve the source's storage format, including B8G8R8A8_TYPELESS.
    // CopyTextureRegion can then copy the engine eye without reinterpretation;
    // the DIBR SRV selects the typed color format afterwards.
    auto source_desc = color_desc;
    source_desc.Width = eye_width;
    source_desc.DepthOrArraySize = 1;
    source_desc.MipLevels = 1;
    source_desc.SampleDesc.Count = 1;
    source_desc.SampleDesc.Quality = 0;
    source_desc.Flags &= ~(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
        D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);
    source_desc.Alignment = 0;

    ComPtr source{};
    if (FAILED(device->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &source_desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            nullptr,
            IID_PPV_ARGS(&source)))) {
        fail("DIBR left-eye staging texture allocation failed");
        return false;
    }

    source->SetName(L"DIBR Source Staging (Left Eye)");
    m_source_staging = std::move(source);
    m_source_staging_width = eye_width;
    m_source_staging_height = color_desc.Height;
    m_source_staging_format = color_desc.Format;
    SPDLOG_INFO(
        "[DIBR] Created left-eye staging source {}x{} storage_format={} srv_format={}",
        m_source_staging_width,
        m_source_staging_height,
        static_cast<uint32_t>(m_source_staging_format),
        static_cast<uint32_t>(color_srv_format(m_source_staging_format)));
    return true;
}

void DIBRPreview::stage_left_eye(
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    D3D12_RESOURCE_STATES color_state)
{
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = color;
    barriers[0].Transition.StateBefore = color_state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource = m_source_staging.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);

    D3D12_TEXTURE_COPY_LOCATION source_location{};
    source_location.pResource = color;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source_location.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination_location{};
    destination_location.pResource = m_source_staging.Get();
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination_location.SubresourceIndex = 0;
    D3D12_BOX source_box{};
    source_box.right = m_source_staging_width;
    source_box.bottom = m_source_staging_height;
    source_box.back = 1;
    command_list->CopyTextureRegion(&destination_location, 0, 0, 0, &source_location, &source_box);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = color_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
}

bool DIBRPreview::create_descriptors(ID3D12Device* device, ID3D12Resource* depth, ID3D12Resource* ui_alpha) {
    if (m_descriptor_heap == nullptr || m_output.texture == nullptr || m_source_staging == nullptr) {
        return false;
    }

    auto cpu = m_descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    const auto color_desc = m_source_staging->GetDesc();
    const auto depth_desc = depth->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC color_srv{};
    color_srv.Format = color_srv_format(color_desc.Format);
    color_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    color_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    color_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_source_staging.Get(), &color_srv, cpu);

    cpu.ptr += m_descriptor_increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv{};
    depth_srv.Format = depth_srv_format(depth_desc.Format);
    depth_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depth_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depth_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(depth, &depth_srv, cpu);

    cpu.ptr += m_descriptor_increment;
    // Bind the owned UI-alpha snapshot only when the caller verified it for
    // this frame. The source staging texture is a harmless fallback because
    // the shader never reads this slot unless the UI guard constant is set.
    auto* ui_resource = ui_alpha != nullptr ? ui_alpha : m_source_staging.Get();
    const auto ui_desc = ui_resource->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC ui_srv{};
    ui_srv.Format = color_srv_format(ui_desc.Format);
    ui_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    ui_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ui_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(ui_resource, &ui_srv, cpu);

    cpu.ptr += m_descriptor_increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC output_uav{};
    output_uav.Format = m_output_format;
    output_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_output.texture.Get(), nullptr, &output_uav, cpu);
    return true;
}

bool DIBRPreview::synthesize(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Resource* color,
    D3D12_RESOURCE_STATES color_state,
    ID3D12Resource* depth,
    D3D12_RESOURCE_STATES depth_state,
    ID3D12Resource* ui_alpha,
    Parameters parameters)
{
    std::string validation_reason{};
    if (device == nullptr || command_list == nullptr || !validate_inputs(color, depth, validation_reason)) {
        if (m_status.load(std::memory_order_acquire) != Status::Failed) {
            bool reason_changed{};
            {
                std::scoped_lock _{m_status_mutex};
                const auto reason = validation_reason.empty() ? "DIBR command context is unavailable" : validation_reason;
                reason_changed = m_failure_reason != reason;
                m_failure_reason = reason;
            }
            m_status.store(Status::Waiting, std::memory_order_release);
            if (reason_changed) {
                SPDLOG_INFO("[DIBR] Waiting: {}", failure_reason());
            }
        }
        return false;
    }

    if (!ensure_pipeline(device)) {
        return false;
    }

    const auto color_desc = color->GetDesc();
    const auto depth_desc = depth->GetDesc();
    const auto output_width = static_cast<uint32_t>(color_desc.Width);
    const auto output_height = color_desc.Height;
    const auto output_format = color_srv_format(color_desc.Format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT output_support{};
    output_support.Format = output_format;
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &output_support, sizeof(output_support))) ||
        (output_support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
        fail("DIBR target format " + std::to_string(static_cast<uint32_t>(output_format)) +
            " does not support typed UAV stores; leaving the normal scene path active");
        return false;
    }
    if (!ensure_source_staging(device, color_desc)) {
        return false;
    }

    // Missing or incompatible UI input merely disables this optional quality
    // pass for the frame. It must never block or alter normal DIBR synthesis.
    if (parameters.ui_edge_guard) {
        const auto ui_valid = ui_alpha != nullptr && [&]() {
            const auto ui_desc = ui_alpha->GetDesc();
            return valid_2d_non_msaa_desc(ui_desc) && ui_desc.MipLevels == 1 &&
                color_srv_format(ui_desc.Format) != DXGI_FORMAT_UNKNOWN &&
                (ui_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;
        }();
        if (!ui_valid) {
            parameters.ui_edge_guard = false;
            parameters.show_ui_edge_guard_mask = false;
        }
    }

    stage_left_eye(command_list, color, color_state);

    if (!ensure_output(device, output_width, output_height, output_format) ||
        !create_descriptors(device, depth, parameters.ui_edge_guard ? ui_alpha : nullptr)) {
        return false;
    }

    Constants constants{};
    constants.output_width = output_width;
    constants.output_height = output_height;
    constants.source_eye_width = m_source_staging_width;
    constants.source_height = output_height;
    constants.depth_width = static_cast<uint32_t>(depth_desc.Width);
    constants.depth_height = depth_desc.Height;
    constants.depth_is_double_wide = depth_desc.Width == color_desc.Width ? 1 : 0;
    constants.reversed_depth = parameters.reversed_depth ? 1 : 0;
    constants.disparity_pixels = std::clamp(parameters.disparity_pixels, 0.0f, 64.0f);
    constants.use_true_reprojection = parameters.use_true_reprojection ? 1 : 0;
    constants.true_reprojection_strength = std::clamp(parameters.reprojection_strength, 0.0f, 2.0f);
    memcpy(constants.source_to_right, parameters.source_to_right.data(), sizeof(constants.source_to_right));
    constants.legacy_depth_curve = std::clamp(parameters.legacy_depth_curve, 0.05f, 4.0f);
    constants.legacy_near_depth_cap = std::clamp(parameters.legacy_near_depth_cap, 0.01f, 1.0f);
    constants.enable_depth_edge_stabilization = parameters.depth_edge_stabilization ? 1 : 0;
    constants.depth_edge_threshold = std::clamp(parameters.depth_edge_threshold, 0.0001f, 0.25f);
    constants.depth_edge_stabilization_strength = std::clamp(parameters.depth_edge_stabilization_strength, 0.0f, 1.0f);
    constants.enable_spatial_repair = parameters.spatial_repair ? 1 : 0;
    constants.show_spatial_repair_mask = parameters.spatial_repair && parameters.show_spatial_repair_mask ? 1 : 0;
    constants.repair_residual_pixels = 1.5f;
    constants.repair_depth_threshold = 0.01f;
    constants.enable_ui_edge_guard = parameters.ui_edge_guard ? 1 : 0;
    constants.show_ui_edge_guard_mask = parameters.ui_edge_guard && parameters.show_ui_edge_guard_mask ? 1 : 0;
    constants.ui_edge_guard_strength = 0.75f;
    memcpy(m_constants_cpu, &constants, sizeof(constants));

    std::array<D3D12_RESOURCE_BARRIER, 2> barriers{};
    for (auto& barrier : barriers) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    barriers[0].Transition.pResource = depth;
    barriers[0].Transition.StateBefore = depth_state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.pResource = m_output.texture.Get();
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Newly allocated output starts as UAV; avoid issuing an invalid first-frame barrier.
    const auto output_was_new = !m_output_has_been_written;
    const auto barrier_count = output_was_new ? 1u : 2u;
    command_list->ResourceBarrier(barrier_count, barriers.data());

    ID3D12DescriptorHeap* heaps[] = {m_descriptor_heap.Get()};
    command_list->SetDescriptorHeaps(1, heaps);
    command_list->SetPipelineState(m_pipeline_state.Get());
    command_list->SetComputeRootSignature(m_root_signature.Get());
    const auto gpu = m_descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    command_list->SetComputeRootDescriptorTable(0, gpu);
    auto uav_gpu = gpu;
    uav_gpu.ptr += static_cast<UINT64>(m_descriptor_increment) * 3;
    command_list->SetComputeRootDescriptorTable(1, uav_gpu);
    command_list->SetComputeRootConstantBufferView(2, m_constants->GetGPUVirtualAddress());
    command_list->Dispatch((output_width + 7) / 8, (output_height + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = m_output.texture.Get();
    command_list->ResourceBarrier(1, &uav_barrier);

    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[0].Transition.StateAfter = depth_state;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers.data());
    m_output_has_been_written = true;
    return true;
}
} // namespace d3d12
