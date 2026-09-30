#version 450

// 2D UI quads. Positions arrive in PIXELS with (0,0) at the top
// left; the push constant's orthographic (orthoRH_ZO with top=0 and
// bottom=height) is what takes them to NDC without flipping anything here.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
// params.x = mode (0 = sprite/flat color, 1 = MSDF)
// params.y = screenPxRange already scaled to the size of THIS quad
// params.z = outline thickness in screen pixels
// effect   = outline color. All per vertex: that way text does not break the batch.
layout(location = 3) in vec4 inParams;
layout(location = 4) in vec4 inEffect;

layout(push_constant) uniform Push {
    mat4 proj;
    // 0 = the destination is SRGB and the hardware converts when writing; 1 = the
    // destination is LINEAR HDR (the scene pass) and the conversion is done by hand in
    // ui.frag. It is not read here, but the block has to be DECLARED THE SAME in
    // both stages: the push constant is ONE SINGLE one for vertex and fragment, and an
    // offset mismatch between them gives neither an error nor a validation warning.
    int linearOutput;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vParams;
layout(location = 3) out vec4 vEffect;

void main()
{
    gl_Position = pc.proj * vec4(inPos, 0.0, 1.0);
    vUv     = inUv;
    vColor  = inColor;
    vParams = inParams;
    vEffect = inEffect;
}
