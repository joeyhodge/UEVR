// Quad View compositor: draws the atlas views back into a plain stereo image.
//
// Used through DirectXTK's SpriteBatch, so the root signature and the vertex
// shader match SpriteBatch's own. Each sprite carries its parameters in the
// vertex data SpriteBatch already has:
//   color       = destination rectangle in pixels (x0, y0, x1, y1)
//   layerDepth  = (shape * 2 + outline + blend * 2) / 8, blend share 0..0.5,
//                 shape 0 rectangle, 1 rounded rectangle, 2 ellipse
// SV_Position gives the pixel being shaded, so the shader knows where it sits
// inside its sprite without an extra constant buffer.
//
// The wide sprites are drawn as opaque rectangles; the sharp sprites are cut
// to the chosen shape and fade to transparent over the outer `blend` share
// (the pipeline uses non-premultiplied alpha blending). Same math as
// uevr::quadview::shape_alpha.

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

    // Same packing as uevr::quadview::pack_sprite_settings.
    float v = position.z * 8.0;
    float code = floor(v + 0.0005);
    float blend = saturate(v - code) * 0.5;
    bool outline = fmod(code, 2.0) >= 0.5;
    int shape = (int)(code * 0.5 + 0.25);

    if (shape == 0 && blend <= 0.0 && !outline) {
        return float4(tex.rgb, 1.0); // wide views: plain opaque copy
    }

    // Distance to the shape's edge as a share of the sprite size (0 on the
    // edge, 0.5 in the centre), same as uevr::quadview::shape_edge_share.
    float2 size = max(color.zw - color.xy, float2(1.0, 1.0));
    float2 uv = (position.xy - color.xy) / size;
    float2 p = abs(uv - 0.5) * 2.0;
    float edge_share;
    if (shape == 2) {
        edge_share = (1.0 - length(p)) * 0.5;
    } else if (shape == 1) {
        const float r = 0.5;
        float2 q = p - (1.0 - r);
        edge_share = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r) * 0.5;
    } else {
        edge_share = min(1.0 - p.x, 1.0 - p.y) * 0.5;
    }

    if (edge_share <= 0.0) {
        return float4(tex.rgb, 0.0); // outside the shape (rounded corners, ellipse)
    }

    // Outline: about 3 pixels inside the edge.
    float edge_px = (shape == 0) ? min(min(uv.x, 1.0 - uv.x) * size.x, min(uv.y, 1.0 - uv.y) * size.y)
                                 : edge_share * min(size.x, size.y);
    if (outline && edge_px < 3.0) {
        return float4(0.1, 1.0, 0.3, 1.0);
    }

    float alpha = 1.0;
    if (blend > 0.0) {
        alpha = smoothstep(0.0, 1.0, saturate(edge_share / blend));
    }

    return float4(tex.rgb, alpha);
}
