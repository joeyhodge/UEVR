#pragma once
// HDR10 -> sRGB for the SWGR HMD scene. Used only when the engine writes PQ into the scene target.
// Optional tuning in <profile>/swgr_hdr.txt: "paper=200 knee=0.75".

#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <optional>
#include <utility>
#include <vector>
#include <sdk/CVar.hpp>
#include "SwgrHdrPolicy.hpp"

namespace swgr_hdr {
struct Params { float paper{200.0f}, knee{0.75f}; };

inline Params load_params(const std::filesystem::path& file) {
    Params p{};
    if (FILE* f = _wfopen(file.c_str(), L"r")) {
        float a{}, b{};
        if (fscanf(f, " paper=%f knee=%f", &a, &b) == 2 && a > 1 && b > 0 && b < 1) { p = {a, b}; }
        fclose(f);
    }
    return p;
}

// Both stages built with fxc; the PSO can't mix DXBC and DXIL.
inline std::pair<std::vector<uint8_t>, std::vector<uint8_t>> compile(const Params& p, std::string& error) {
    char src[3600];
    snprintf(src, sizeof(src), R"HLSL(
#define RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT|DENY_DOMAIN_SHADER_ROOT_ACCESS|DENY_GEOMETRY_SHADER_ROOT_ACCESS|DENY_HULL_SHADER_ROOT_ACCESS), DescriptorTable(SRV(t0), visibility=SHADER_VISIBILITY_PIXEL), CBV(b0), StaticSampler(s0, filter=FILTER_MIN_MAG_MIP_LINEAR, addressU=TEXTURE_ADDRESS_CLAMP, addressV=TEXTURE_ADDRESS_CLAMP, addressW=TEXTURE_ADDRESS_CLAMP, visibility=SHADER_VISIBILITY_PIXEL)"
Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);
cbuffer Parameters : register(b0) { row_major float4x4 MatrixTransform; };
[RootSignature(RS)]
void SpriteVertexShader(inout float4 color : COLOR0, inout float2 texCoord : TEXCOORD0, inout float4 position : SV_Position) {
    position = mul(position, MatrixTransform);
}
static const float PAPER = %f, KNEE = %f;
float3 pq_to_nits(float3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(saturate(n), 1.0 / m2);
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), 1.0 / m1);
}
float3 srgb_oetf(float3 l) {
    return l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}
[RootSignature(RS)]
float4 SpritePixelShader(float4 color : COLOR0, float2 uv : TEXCOORD0) : SV_Target0 {
    float3 c2020 = pq_to_nits(Texture.Sample(TextureSampler, uv).rgb) / PAPER;
    const float3x3 to709 = float3x3( 1.6605, -0.5876, -0.0728,
                                    -0.1246,  1.1329, -0.0083,
                                    -0.0182, -0.1006,  1.1187);
    float3 c = max(mul(to709, c2020), 0.0);
    // Soft shoulder above KNEE.
    float m = max(c.r, max(c.g, c.b));
    if (m > KNEE) {
        float span = 1.0 - KNEE;
        float mapped = KNEE + span * (1.0 - exp(-(m - KNEE) / span));
        c *= mapped / m;
    }
    return float4(srgb_oetf(saturate(c)), 1.0);
}
)HLSL", p.paper, p.knee);
    auto build = [&](const char* entry, const char* target) -> std::vector<uint8_t> {
        Microsoft::WRL::ComPtr<ID3DBlob> code, err;
        if (FAILED(D3DCompile(src, strlen(src), "swgr_hdr", nullptr, nullptr, entry, target,
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
            error += err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()) : "D3DCompile failed";
            return {};
        }
        auto b = (const uint8_t*)code->GetBufferPointer();
        return {b, b + code->GetBufferSize()};
    };
    auto ps = build("SpritePixelShader", "ps_5_1");
    auto vs = build("SpriteVertexShader", "vs_5_1");
    if (ps.empty() || vs.empty()) { return {}; }
    return {std::move(ps), std::move(vs)};
}

// The engine's HDR output state, which decides how the tonemapper encoded the scene target.
inline std::pair<std::optional<int32_t>, std::optional<int32_t>> read_hdr_cvars() {
    const auto read = [](const wchar_t* name) -> std::optional<int32_t> {
        try {
            const auto manager = sdk::FConsoleManager::get();
            const auto object = manager ? manager->find(name) : nullptr;
            if (!object || !object->is_validated_ue57_variable() || object->AsCommand() != nullptr) { return {}; }
            return static_cast<sdk::IConsoleVariable*>(object)->TryGetInt();
        } catch (...) { return {}; }
    };
    return {read(L"r.HDR.EnableHDROutput"), read(L"r.HDR.Display.OutputDevice")};
}
} // namespace swgr_hdr
