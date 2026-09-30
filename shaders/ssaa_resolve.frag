#version 450

// SSAA downsample: the whole scene has been rendered at m_renderExtent (the
// window size multiplied by the factor) and here it is averaged down to the
// real size of m_offscreenImage.
//
// The average is done in LINEAR, not in gamma: the source is B8G8R8A8_SRGB and the
// sampler already returns the decoded color, which is exactly what has to be
// averaged for the result to be energetically correct. The output attachment
// encodes it again on its own.
layout(location = 0) in  vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec2 invSrc;   // 1/size of the BIG image (the source)
    int  taps;     // samples per axis: 2 for 2x, 3 for 3x...
} push;

void main()
{
    // With a single tap this would be a copy: the linear sampler would already average
    // four texels, but only the four surrounding the exact center, which at
    // factor 2 leaves out part of the destination pixel's footprint.
    if (push.taps <= 1)
    {
        outColor = vec4(texture(srcTex, inUv).rgb, 1.0);
        return;
    }

    // Centered grid of taps x taps within the destination pixel's footprint.
    // The offsets are in texels of the SOURCE image.
    const float n     = float(push.taps);
    const float start = -0.5 * (n - 1.0);

    vec3 sum = vec3(0.0);
    for (int y = 0; y < push.taps; y++)
    {
        for (int x = 0; x < push.taps; x++)
        {
            vec2 off = (vec2(start + float(x), start + float(y))) * push.invSrc;
            sum += texture(srcTex, inUv + off).rgb;
        }
    }

    outColor = vec4(sum / (n * n), 1.0);
}
