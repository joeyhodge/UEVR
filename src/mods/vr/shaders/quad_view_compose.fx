// Quad View compositor (D3D12): draws the atlas views back into a plain
// stereo image.
//
// Used through DirectXTK's SpriteBatch, so the root signature and the vertex
// shader match SpriteBatch's own. The pixel logic (sprite settings packing,
// bicubic wide views, sharpening, shapes, edge blend, outline) is shared with
// the D3D11 compose in quad_view_compose.hlsli. The root signature is
// SpriteBatch's plus four root constants (b1); D3D12Component builds the
// SpriteBatch with it as its custom root signature. The pipeline uses
// non-premultiplied alpha blending; the wide sprites are opaque.

#define SpriteStaticRS \
"RootFlags ( ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |" \
"            DENY_DOMAIN_SHADER_ROOT_ACCESS |" \
"            DENY_GEOMETRY_SHADER_ROOT_ACCESS |" \
"            DENY_HULL_SHADER_ROOT_ACCESS )," \
"DescriptorTable ( SRV(t0), visibility = SHADER_VISIBILITY_PIXEL ),"\
"CBV(b0), "\
"RootConstants(num32BitConstants = 4, b1, visibility = SHADER_VISIBILITY_PIXEL), "\
"StaticSampler(s0,"\
"           filter = FILTER_MIN_MAG_MIP_LINEAR,"\
"           addressU = TEXTURE_ADDRESS_CLAMP,"\
"           addressV = TEXTURE_ADDRESS_CLAMP,"\
"           addressW = TEXTURE_ADDRESS_CLAMP,"\
"           visibility = SHADER_VISIBILITY_PIXEL )"

Texture2D<float4> Texture : register(t0);
sampler TextureSampler : register(s0);

cbuffer Parameters : register(b0)
{
    row_major float4x4 MatrixTransform;
};

// Per-compose constants, set by QuadViewCompositor as root constants (root
// parameter 2; SpriteBatch owns parameters 0 and 1).
cbuffer QuadComposeConstants : register(b1)
{
    float Brightness; // shared-exposure brightness multiplier
    float3 Padding;
};

#define QV_BRIGHTNESS Brightness

#include "quad_view_compose.hlsli"

[RootSignature(SpriteStaticRS)]
void SpriteVertexShader(inout float4 color    : COLOR0,
    inout float2 texCoord : TEXCOORD0,
    inout float4 position : SV_Position)
{
    position = mul(position, MatrixTransform);
}

[RootSignature(SpriteStaticRS)]
float4 SpritePixelShader(float4 color    : COLOR0,
    float2 texCoord : TEXCOORD0,
    float4 position : SV_Position) : SV_Target0
{
    return qv_compose(color, texCoord, position);
}
