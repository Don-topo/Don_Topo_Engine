#version 450

// TAA: temporal accumulation. The scene pass has been rendered with a different
// subpixel jitter each frame (Halton sequence), so averaging this
// frame with the history is equivalent to supersampling spread over time.
//
// There is NO velocity buffer in the engine, so the reprojection is by depth
// only: it is right for static geometry and with the camera moving,
// which is the case that produces almost all the visible stair-stepping. Objects that
// move by themselves (and skinned meshes) reproject to the wrong
// position; what keeps that from turning into a persistent trail is the
// clamp to the neighborhood below, which ties the history to the colors that
// really are around the pixel in this frame.
layout(location = 0) in  vec2 inUv;
// Two destinations at once: the image that is presented and the history that the next
// frame will read. It is the same color, written only once.
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;

layout(set = 0, binding = 0) uniform sampler2D currentTex;
layout(set = 0, binding = 1) uniform sampler2D historyTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

layout(push_constant) uniform Push {
    // Clip of THIS frame (without jitter) -> clip of the previous one. It is computed on the CPU
    // as prevViewProj * inverse(currViewProj) so as not to spend two mat4.
    mat4  reproject;
    vec2  invRes;
    float feedback;      // history weight: 0.9 = 90% history, 10% new frame
    int   historyValid;  // 0 on the first frame, after a resize or when changing mode
} push;

void main()
{
    const vec3 current = texture(currentTex, inUv).rgb;

    if (push.historyValid == 0)
    {
        outColor   = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    // Reprojection: from (uv, depth) of this frame to the uv that this same
    // point occupied in the previous frame. The depth comes from the depth pre-pass, which
    // is recorded WITHOUT jitter, so the reconstructed position is the geometric one.
    const float depth = texture(depthTex, inUv).r;

    vec4 clip = vec4(inUv * 2.0 - 1.0, depth, 1.0);
    vec4 prev = push.reproject * clip;
    // Far background (depth == 1.0): w may end up degenerate. It is treated as
    // invalid history instead of dividing by almost zero.
    vec2 prevUv = (abs(prev.w) > 1e-6) ? (prev.xy / prev.w) * 0.5 + 0.5 : vec2(-1.0);

    // Off screen in the previous frame: there was nothing to accumulate there.
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0)
    {
        outColor   = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(historyTex, prevUv).rgb;

    // Clamp to the 3x3 neighborhood: the history is limited to the color box
    // surrounding the pixel IN THIS FRAME. It is what cuts ghosting when the
    // reprojection fails (moving objects, disocclusions): if the old color does not
    // resemble anything of what is around now, it is clamped until it does.
    vec3 boxMin = current;
    vec3 boxMax = current;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            if (x == 0 && y == 0) continue;
            vec3 c = texture(currentTex, inUv + vec2(x, y) * push.invRes).rgb;
            boxMin = min(boxMin, c);
            boxMax = max(boxMax, c);
        }
    }
    history = clamp(history, boxMin, boxMax);

    const vec3 result = mix(current, history, push.feedback);

    outColor   = vec4(result, 1.0);
    outHistory = vec4(result, 1.0);
}
