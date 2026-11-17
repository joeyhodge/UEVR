// Quad View compose: the pixel logic shared by the D3D12 compositor
// (quad_view_compose.fx, DXC) and the D3D11 compose
// (quad_view_compose_d3d11.fx, FXC). Both draw the atlas through DirectXTK's
// SpriteBatch, and each sprite carries its parameters in vertex data
// SpriteBatch already has:
//   color       = destination rectangle in pixels (x0, y0, x1, y1)
//   layerDepth  = settings packed by uevr::quadview::pack_sprite_settings:
//                 (code + blend * 2) / 128, code = base + 8 * sharpen level,
//                 base 0..5 = shape * 2 + outline for a sharp sprite,
//                 base 6 = wide sprite with bicubic upscaling,
//                 sharpen level 0..10 (tenths), blend share 0..0.5
// SV_Position gives the pixel being shaded, so the shader knows where it sits
// inside its sprite without an extra constant buffer.
//
// The including file declares Texture, TextureSampler and QV_BRIGHTNESS.

static const float QV_PACK_SCALE = 128.0;
static const float QV_WIDE_BASE = 6.0;

struct QVSprite {
    float blend;
    bool outline;
    int shape;      // 0 rectangle, 1 rounded rectangle, 2 ellipse
    bool wide;      // wide view: bicubic upscale, opaque
    float sharpen;  // 0..1
};

QVSprite qv_unpack(float depth) {
    // Same packing as uevr::quadview::pack_sprite_settings.
    float v = depth * QV_PACK_SCALE;
    float code = floor(v + 0.0005);
    QVSprite s = (QVSprite)0;
    s.blend = saturate(v - code) * 0.5;
    float level = floor(code / 8.0 + 0.001);
    float base = code - level * 8.0;
    s.sharpen = level * 0.1;
    s.wide = base >= QV_WIDE_BASE - 0.5;
    s.outline = !s.wide && fmod(base, 2.0) >= 0.5;
    s.shape = s.wide ? 0 : (int)(base * 0.5 + 0.25);
    return s;
}

// The sprite's source rectangle in texture coordinates, from how the texture
// coordinate changes per pixel. Samples are clamped to it so the filters
// never pull in the neighbouring view of the atlas.
void qv_source_bounds(float2 uv, float4 position, float4 dest, out float2 lo, out float2 hi) {
    float2 duv = float2(ddx(uv.x), ddy(uv.y));
    lo = uv - (position.xy - dest.xy) * duv;
    hi = lo + (dest.zw - dest.xy) * duv;
    float2 a = min(lo, hi);
    float2 b = max(lo, hi);
    lo = a;
    hi = b;
}

float4 qv_sample(float2 uv, float2 lo, float2 hi) {
    return Texture.SampleLevel(TextureSampler, clamp(uv, lo, hi), 0);
}

// Catmull-Rom bicubic from nine bilinear taps (the usual 4x4 kernel folded
// into the hardware filter). The wide views are stretched about five times, so
// this keeps the periphery from shimmering the way plain bilinear does.
float4 qv_bicubic(float2 uv, float2 size, float2 lo, float2 hi) {
    float2 p = uv * size;
    float2 t1 = floor(p - 0.5) + 0.5;
    float2 f = p - t1;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float2 w12 = w1 + w2;
    float2 t12 = (t1 + w2 / w12) / size;
    float2 t0 = (t1 - 1.0) / size;
    float2 t3 = (t1 + 2.0) / size;

    float4 r = 0;
    r += qv_sample(float2(t0.x,  t0.y),  lo, hi) * w0.x  * w0.y;
    r += qv_sample(float2(t12.x, t0.y),  lo, hi) * w12.x * w0.y;
    r += qv_sample(float2(t3.x,  t0.y),  lo, hi) * w3.x  * w0.y;
    r += qv_sample(float2(t0.x,  t12.y), lo, hi) * w0.x  * w12.y;
    r += qv_sample(float2(t12.x, t12.y), lo, hi) * w12.x * w12.y;
    r += qv_sample(float2(t3.x,  t12.y), lo, hi) * w3.x  * w12.y;
    r += qv_sample(float2(t0.x,  t3.y),  lo, hi) * w0.x  * w3.y;
    r += qv_sample(float2(t12.x, t3.y),  lo, hi) * w12.x * w3.y;
    r += qv_sample(float2(t3.x,  t3.y),  lo, hi) * w3.x  * w3.y;
    return max(r, 0.0); // the kernel can undershoot slightly at hard edges
}

// Contrast-adaptive sharpening (after AMD FidelityFX CAS): sharpens flat
// detail more than edges that already have contrast, so it adds crispness
// without halos. amount 0..1.
float3 qv_sharpen(float2 uv, float2 size, float2 lo, float2 hi, float amount) {
    float2 t = 1.0 / size;
    float3 c = qv_sample(uv, lo, hi).rgb;
    float3 n = qv_sample(uv + float2(0, -t.y), lo, hi).rgb;
    float3 s = qv_sample(uv + float2(0,  t.y), lo, hi).rgb;
    float3 w = qv_sample(uv + float2(-t.x, 0), lo, hi).rgb;
    float3 e = qv_sample(uv + float2( t.x, 0), lo, hi).rgb;

    // Contrast is judged on display-range values so HDR highlights do not
    // switch the sharpening off.
    float3 mn = min(saturate(c), min(min(saturate(n), saturate(s)), min(saturate(w), saturate(e))));
    float3 mx = max(saturate(c), max(max(saturate(n), saturate(s)), max(saturate(w), saturate(e))));
    float3 amp = sqrt(saturate(min(mn, 1.0 - mx) / max(mx, 1e-5)));
    float3 k = amp * (-1.0 / lerp(8.0, 5.0, saturate(amount)));
    return max((c + (n + s + w + e) * k) / (1.0 + 4.0 * k), 0.0);
}

float4 qv_compose(float4 color, float2 texCoord, float4 position) {
    QVSprite sp = qv_unpack(position.z);

    float2 size = float2(1.0, 1.0);
    Texture.GetDimensions(size.x, size.y);
    float2 lo = 0.0;
    float2 hi = 1.0;
    qv_source_bounds(texCoord, position, color, lo, hi);
    // Keep bilinear taps half a texel inside the view.
    float2 half_texel = 0.5 / size;
    lo += half_texel;
    hi = max(hi - half_texel, lo);

    // One exit (FXC rejects early returns here as possibly uninitialised).
    float4 result = float4(0.0, 0.0, 0.0, 1.0);

    if (sp.wide) {
        result = float4(qv_bicubic(texCoord, size, lo, hi).rgb * QV_BRIGHTNESS, 1.0);
    } else {
        float3 rgb = qv_sample(texCoord, lo, hi).rgb;
        if (sp.sharpen > 0.0) {
            rgb = qv_sharpen(texCoord, size, lo, hi, sp.sharpen);
        }
        rgb *= QV_BRIGHTNESS;

        // Distance to the shape's edge as a share of the sprite size (0 on the
        // edge, 0.5 in the centre), same as uevr::quadview::shape_edge_share.
        float2 dest_size = max(color.zw - color.xy, float2(1.0, 1.0));
        float2 suv = (position.xy - color.xy) / dest_size;
        float2 p = abs(suv - 0.5) * 2.0;
        float edge_share = min(1.0 - p.x, 1.0 - p.y) * 0.5;
        if (sp.shape == 2) {
            edge_share = (1.0 - length(p)) * 0.5;
        } else if (sp.shape == 1) {
            const float r = 0.5;
            float2 q = p - (1.0 - r);
            edge_share = -(length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r) * 0.5;
        }

        // Outline: about 3 pixels inside the edge.
        float edge_px = (sp.shape == 0)
            ? min(min(suv.x, 1.0 - suv.x) * dest_size.x, min(suv.y, 1.0 - suv.y) * dest_size.y)
            : edge_share * min(dest_size.x, dest_size.y);

        float alpha = 1.0;
        if (sp.blend > 0.0) {
            alpha = smoothstep(0.0, 1.0, saturate(edge_share / sp.blend));
        }

        if (edge_share <= 0.0) {
            result = float4(rgb, 0.0); // outside the shape (rounded corners, ellipse)
        } else if (sp.outline && edge_px < 3.0) {
            result = float4(0.1, 1.0, 0.3, 1.0);
        } else {
            result = float4(rgb, alpha);
        }
    }

    return result;
}
