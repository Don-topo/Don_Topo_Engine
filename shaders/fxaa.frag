#version 450

// FXAA 3.11 (PC quality preset, 12 search steps). It runs AFTER the composition, on the already
// tonemapped LDR image: it is a post filter that needs the final color, not the linear HDR.
//
// m_fxaaSrcImage comes in (what the composition used to write directly into m_offscreenImage,
// selection outline and gizmos included) and m_offscreenImage goes out, which is the one the editor
// UI samples and the one the headless runtime blits. With the effect off this pass is NOT recorded
// and the composition goes back to writing directly into m_offscreenImage: byte-for-byte identical
// image.
layout(location = 0) in  vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ldrTex;

layout(push_constant) uniform Push {
    vec2  invRes;             // 1/width, 1/height of the viewport
    float subpix;             // strength of the subpixel filter (0 = edges only)
    float edgeThreshold;      // minimum relative contrast to treat something as an edge
    float edgeThresholdMin;   // minimum absolute contrast (cuts noise in dark areas)
} push;

// Perceptual luma. m_offscreenImage is B8G8R8A8_SRGB, so the sampler already
// returns the color DECODED to linear: the return to gamma is approximated with
// sqrt before weighting the channels. FXAA detects edges on non-linear luma; if
// measured in linear, the edges in the dark half of the image fall below the
// threshold and are not smoothed.
float luma(vec3 c) { return dot(sqrt(c), vec3(0.299, 0.587, 0.114));  }

// Length of each jump of the edge-end search. The first
// steps are one texel (precision near the pixel) and the last ones get longer
// to reach very long edges without spending iterations.
const float kStep[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

void main()
{
    const vec2 rcp = push.invRes;

    const vec3  rgbM  = texture(ldrTex, inUv).rgb;
    const float lumaM = luma(rgbM);

    // Cross of neighbors: decides whether there is an edge here and with what contrast.
    const float lumaN = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 0, -1)).rgb);
    const float lumaS = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 0,  1)).rgb);
    const float lumaW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1,  0)).rgb);
    const float lumaE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1,  0)).rgb);

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range   = lumaMax - lumaMin;

    // Flat area: the pixel is returned untouched. It is what makes the interior of
    // surfaces come out exactly the same as without FXAA.
    if (range < max(push.edgeThresholdMin, lumaMax * push.edgeThreshold))
    {
        outColor = vec4(rgbM, 1.0);
        return;
    }

    // Diagonals: needed to decide the ORIENTATION of the edge.
    const float lumaNW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1, -1)).rgb);
    const float lumaNE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1, -1)).rgb);
    const float lumaSW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1,  1)).rgb);
    const float lumaSE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1,  1)).rgb);

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;

    // Second derivative on each axis: the axis on which the color changes LEAST wins,
    // which is the one along which the edge runs.
    const float edgeHorz = abs(-2.0 * lumaW + lumaNW + lumaSW)
                         + abs(-2.0 * lumaM + lumaNS) * 2.0
                         + abs(-2.0 * lumaE + lumaNE + lumaSE);
    const float edgeVert = abs(-2.0 * lumaN + lumaNW + lumaNE)
                         + abs(-2.0 * lumaM + lumaWE) * 2.0
                         + abs(-2.0 * lumaS + lumaSW + lumaSE);

    const bool horzSpan = edgeHorz >= edgeVert;

    // The two neighbors perpendicular to the edge and their gradient.
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float grad1 = luma1 - lumaM;
    float grad2 = luma2 - lumaM;

    // Of the two sides, the one with the stronger jump is the "real" edge.
    const bool  steepest    = abs(grad1) >= abs(grad2);
    const float gradScaled  = 0.25 * max(abs(grad1), abs(grad2));

    // It advances half a texel towards the edge to sample right on top of it.
    float stepLength = horzSpan ? rcp.y : rcp.x;
    float lumaLocal  = 0.0;
    if (steepest)
    {
        stepLength = -stepLength;
        lumaLocal  = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocal  = 0.5 * (luma2 + lumaM);
    }

    vec2 currentUv = inUv;
    if (horzSpan) currentUv.y += stepLength * 0.5;
    else          currentUv.x += stepLength * 0.5;

    // Walk along the edge in both directions until finding its
    // ends: it is what distinguishes FXAA from a simple 3x3 blur.
    const vec2 offset = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 uv1 = currentUv - offset * kStep[0];
    vec2 uv2 = currentUv + offset * kStep[0];

    float lumaEnd1 = luma(texture(ldrTex, uv1).rgb) - lumaLocal;
    float lumaEnd2 = luma(texture(ldrTex, uv2).rgb) - lumaLocal;

    bool reached1 = abs(lumaEnd1) >= gradScaled;
    bool reached2 = abs(lumaEnd2) >= gradScaled;

    if (!reached1) uv1 -= offset * kStep[1];
    if (!reached2) uv2 += offset * kStep[1];

    if (!reached1 || !reached2)
    {
        for (int i = 2; i < 12; i++)
        {
            if (!reached1) lumaEnd1 = luma(texture(ldrTex, uv1).rgb) - lumaLocal;
            if (!reached2) lumaEnd2 = luma(texture(ldrTex, uv2).rgb) - lumaLocal;

            reached1 = reached1 || abs(lumaEnd1) >= gradScaled;
            reached2 = reached2 || abs(lumaEnd2) >= gradScaled;

            if (reached1 && reached2) break;

            if (!reached1) uv1 -= offset * kStep[i];
            if (!reached2) uv2 += offset * kStep[i];
        }
    }

    // Distance to each end: the closer the pixel is to an end,
    // the less it has to be shifted.
    const float dist1 = horzSpan ? (inUv.x - uv1.x) : (inUv.y - uv1.y);
    const float dist2 = horzSpan ? (uv2.x - inUv.x) : (uv2.y - inUv.y);

    const bool  nearest1  = dist1 < dist2;
    const float distFinal = min(dist1, dist2);
    const float spanLen   = dist1 + dist2;

    float pixelOffset = -distFinal / spanLen + 0.5;

    // If the nearest end changes sign with respect to the pixel, the edge does not
    // pass through here and shifting would be blurring too much.
    const bool  lumaMSmaller = lumaM < lumaLocal;
    const bool  goodSpan     = ((nearest1 ? lumaEnd1 : lumaEnd2) < 0.0) != lumaMSmaller;
    float finalOffset = goodSpan ? pixelOffset : 0.0;

    // Subpixel filter: average of the 3x3 against the pixel, for details finer
    // than a complete edge (wires, grilles, the selection outline).
    const float lumaAvg   = (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaNW + lumaNE + lumaSW + lumaSE);
    const float subDelta  = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
    const float subSmooth = (-2.0 * subDelta + 3.0) * subDelta * subDelta;
    const float subOffset = subSmooth * subSmooth * push.subpix;

    finalOffset = max(finalOffset, subOffset);

    vec2 finalUv = inUv;
    if (horzSpan) finalUv.y += finalOffset * stepLength;
    else          finalUv.x += finalOffset * stepLength;

    // The alpha is forced to 1.0 as in the composition: it is what the editor UI
    // expects when sampling this image and the blit to the swapchain.
    outColor = vec4(texture(ldrTex, finalUv).rgb, 1.0);
}
