#version 450

layout(location = 0) in vec3 inPos;

#include "shadow_config.glsl"
// Shadow matrix slots. The first 6 belong to the KEY light (4 cascades,
// or 6 cubemap faces, or 1 spot face); the 4 after them are one secondary
// spot each. Same value as SHADOW_MATRICES in
// UniformBufferObject.h.

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix[SHADOW_MATRICES];
} ubo;

// Which layer of the texture array is being recorded. The shadow pipeline's own range:
// it does not share pipeline layout with triangle/pbr/outline, so the
// PushData block of those is not touched.
layout(push_constant) uniform ShadowPush {
    uint cascade;
} push;

// Same per-frame SSBO as triangle.vert (set 1, binding 0), but with its own
// range: the shadow pass culls with the LIGHT's frustum, so the visible
// set is not the camera's and its transforms go in another section of the buffer.
// This pass only draws grouped static objects, so there is no push
// constant path to preserve.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

void main()
{
    gl_Position = ubo.lightSpaceMatrix[push.cascade] * instances.models[gl_InstanceIndex] * vec4(inPos, 1.0);
}