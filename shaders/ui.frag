#version 450

// The sprite atlas is an SRGB image: the sampler already returns linear and the
// UI pass attachment (B8G8R8A8_SRGB) reconverts when writing. On that
// path no extra gamma correction has to be applied here, but on
// the SCENE pass's path it does, and that is what pc.linearOutput is about (see the end of main).
//
// A FONT's atlas, on the other hand, is UNORM: an MSDF is distances, not
// color, and sampling it through an SRGB view distorts them without a single validation
// warning. It is UiFont that declares it, not this shader.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

// The SAME block that ui.vert declares, member by member: the push constant is
// a single one for both stages. Here only linearOutput is read, but the mat4
// has to be declared in front or the flag would land at another offset, and that
// gives neither a compile error nor a validation warning, just a flag with garbage.
layout(push_constant) uniform Push {
    mat4 proj;
    int  linearOutput;
} pc;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec4 vEffect;

layout(location = 0) out vec4 outColor;

// The median of the three channels is the reconstructed signed distance: it is what
// makes corners remain corners when magnifying.
float median3(vec3 c)
{
    return max(min(c.r, c.g), min(max(c.r, c.g), c.b));
}

void main()
{
    vec4 tex = texture(uAtlas, vUv);

    // The color is computed into a local and written ONCE at the end: the
    // gamma correction below has to go through the three paths, and with
    // a `return` per branch it would be forgotten in two of them without anything warning.
    vec4 color;

    // Mode 0: exactly as always. A sprite or flat color quad
    // comes out the same as before text existed, and in the same batch.
    if (vParams.x < 0.5)
    {
        // Straight alpha: the outside blending does SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
        // so the color is NOT premultiplied by the alpha here.
        color = tex * vColor;
    }
    else
    {
        // Distance in SCREEN PIXELS: 0.5 is the edge and screenPxRange converts
        // the MSDF's normalized range to the size the quad is being drawn at.
        float px = vParams.y * (median3(tex.rgb) - 0.5);

        float fill = clamp(px + 0.5, 0.0, 1.0);

        if (vParams.z > 0.0)
        {
            // The outline is the SAME distance shifted: neither a second texture nor
            // re-baking anything.
            float outer = clamp(px + vParams.z + 0.5, 0.0, 1.0);
            vec3  rgb   = mix(vEffect.rgb, vColor.rgb, fill);
            float alpha = mix(vEffect.a * outer, vColor.a, fill);
            color = vec4(rgb, alpha);
        }
        else
        {
            color = vec4(vColor.rgb, vColor.a * fill);
        }
    }

    // The UI pass writes to an SRGB attachment: the hardware encodes when
    // writing, so there this number IS ALREADY the linear light that is seen.
    //
    // The SCENE pass does not: it is LINEAR HDR (kHdrFormat) and everything written
    // there later goes through bloom_composite.frag (ACES + pow(1/2.2)). Writing
    // the same number makes it come out WASHED OUT: a 0.5 ends up at ~0.80 on screen, not at
    // 0.5. Undoing the gamma here puts it back in its place (~0.60); what remains
    // as a difference is the tonemap, and THAT is desirable: a sign that is in the
    // world has to be exposed like the rest of the scene. No validation
    // layer says a word about this, the symptom is only the color.
    //
    // The max() is because pow() with a negative base is undefined behavior.
    if (pc.linearOutput != 0)
        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(2.2));

    outColor = color;
}
