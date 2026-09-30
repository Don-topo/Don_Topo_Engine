#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec3 inTangent;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV; 
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec3 fragTangent;
layout(location = 5) out vec3 fragBitangent;
// There is no light-space position varying: with cascades N would be needed, and
// the fragment shader already reconstructs the right one from fragWorldPos.

#include "shadow_config.glsl"
// Shadow matrix slots. The first 6 belong to the KEY light (4 cascades,
// or 6 cubemap faces, or 1 spot face); the 4 after them are one secondary
// spot each. Same value as SHADOW_MATRICES in
// UniformBufferObject.h.

layout(set = 0, binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix[SHADOW_MATRICES];
} ubo;

// Per-instance transforms, one per frame-in-flight. Static objects that
// share mesh+material are drawn in a single instanced draw and each
// instance takes its matrix from here by gl_InstanceIndex.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

// Same types and offsets as the pbr.frag block (the two stages of the same
// pipeline share the push constants range): mat4 + 2 float + vec2. The
// slot of that vec2 was padding; now its .x carries the instancing flag and its .y
// is still unused, so no offset has moved and pbr.frag does not change.
layout(push_constant) uniform PushData
{
    mat4  transform;
    float metallic;
    float roughness;
    vec2  flags;      // x: 1 = take the model matrix from the instance SSBO
} push;

void main()
{
    // useInstancing == 0 is the skinned path: it shares this vertex shader and this
    // pipeline layout, draws a single instance and brings its matrix in the push
    // constant, not in the SSBO.
    mat4 model = push.flags.x != 0.0 ? instances.models[gl_InstanceIndex] : push.transform;

    gl_Position = ubo.proj * ubo.view * model * vec4(inPos, 1.0);
    fragColor   = inColor;
    fragUV      = inUV;
    fragNormal   = mat3(model) * inNormal;
    fragWorldPos = vec3(model * vec4(inPos, 1.0));
    vec3 T = normalize(mat3(model) * inTangent);
    vec3 N = normalize(mat3(model) * inNormal);
    T = normalize(T - dot(T, N) * N);
    fragTangent   = T;
    fragBitangent = cross(N, T);
}