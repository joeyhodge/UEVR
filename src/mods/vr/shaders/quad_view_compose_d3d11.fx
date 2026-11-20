// Quad View compose (D3D11): the pixel shader used with DirectXTK's D3D11
// SpriteBatch. Compiled with FXC (shader model 5.0) by compile_shaders.bat;
// the pixel logic is shared with the D3D12 compositor in
// quad_view_compose.hlsli, so both paths draw the same image.

Texture2D<float4> Texture : register(t0);
SamplerState TextureSampler : register(s0);

cbuffer QuadComposeConstants : register(b0)
{
    float Brightness; // shared-exposure brightness multiplier
    float3 Padding;
};

#define QV_BRIGHTNESS Brightness

#include "quad_view_compose.hlsli"

float4 SpritePixelShader(float4 color : COLOR0, float2 texCoord : TEXCOORD0, float4 position : SV_Position) : SV_Target
{
    return qv_compose(color, texCoord, position);
}
