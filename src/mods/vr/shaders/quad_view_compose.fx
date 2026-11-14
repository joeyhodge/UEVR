// Quad View compositor: draws the atlas views back into a plain stereo image.
//
// Used through DirectXTK's SpriteBatch, so the root signature and the vertex
// shader match SpriteBatch's own. Each sprite carries its parameters in the
// vertex data SpriteBatch already has:
//   color       = destination rectangle in pixels (x0, y0, x1, y1)
//   layerDepth  = edge blend share (0..0.5) * 0.5, plus 0.5 when the outline is on
// SV_Position gives the pixel being shaded, so the shader knows where it sits
// inside its sprite without an extra constant buffer.
//
// The wide sprites are drawn with blend 0 (opaque); the sharp sprites fade to
// transparent over the outer `blend` share of each side (the pipeline uses
// non-premultiplied alpha blending). Same math as uevr::quadview::edge_alpha.

#define SpriteStaticRS \
"RootFlags ( ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |" \
"            DENY_DOMAIN_SHADER_ROOT_ACCESS |" \
"            DENY_GEOMETRY_SHADER_ROOT_ACCESS |" \
"            DENY_HULL_SHADER_ROOT_ACCESS )," \
"DescriptorTable ( SRV(t0), visibility = SHADER_VISIBILITY_PIXEL ),"\
"CBV(b0), "\
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
    float4 tex = Texture.Sample(TextureSampler, texCoord);

    float packed = position.z;
    bool outline = packed >= 0.5;
    float blend = (packed - (outline ? 0.5 : 0.0)) * 2.0;

    // Distance to the nearest sprite edge, in pixels and as a share of the sprite.
    float2 lo = position.xy - color.xy;
    float2 hi = color.zw - position.xy;
    float2 size = max(color.zw - color.xy, float2(1.0, 1.0));
    float edge_px = min(min(lo.x, hi.x), min(lo.y, hi.y));
    float edge_share = min(min(lo.x, hi.x) / size.x, min(lo.y, hi.y) / size.y);

    if (outline && edge_px < 3.0) {
        return float4(0.1, 1.0, 0.3, 1.0);
    }

    float alpha = 1.0;
    if (blend > 0.0) {
        alpha = smoothstep(0.0, 1.0, saturate(edge_share / blend));
    }

    return float4(tex.rgb, alpha);
}
