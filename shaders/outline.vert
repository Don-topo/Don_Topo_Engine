#version 450

// Inverted hull of the object selected in the editor: the same mesh
// extruded along its normal, drawn with front faces
// culled. Only the rim that lies outside the original silhouette shows;
// the rest of the hull falls behind the geometry and is discarded by the depth
// test. It shares pipeline layout with triangle.vert (same UBO in set 0 and the
// same push constants block), so it needs neither descriptor sets nor
// ranges of its own.

layout(location = 0) in vec3 inPos;
// Locations 1 (color), 2 (uv) and 4 (tangent) of the vertex input exist in
// the pipeline but are not declared here: a shader is not obliged to
// consume all the attributes of the binding.
layout(location = 3) in vec3 inNormal;

// The cascade array is not used here, but it is declared the same as in the other
// set 0 shaders: if the block were left with a single mat4, any
// member added behind it would read from a different offset than the one the C++
// UBO writes.
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

// Same layout as the triangle.vert/pbr.frag block: mat4 + 2 float + vec2.
// flags.x is not used here (the outline draws ONE object with its matrix in the push,
// never through instancing); flags.y carries the extrusion thickness in world
// units, which was the free slot of that vec2.
layout(push_constant) uniform PushData
{
    mat4  transform;
    float metallic;
    float roughness;
    vec2  flags;      // y: outline thickness (world)
} push;

void main()
{
    vec4 worldPos = push.transform * vec4(inPos, 1.0);

    // Normal to world WITHOUT normalizing first: mat3(transform) may carry
    // scale. If it comes out with zero length (mesh without normals) normalize would give
    // NaN and the whole triangle would disappear, so in that case it is not
    // extruded and the hull coincides with the mesh (no outline is seen, but
    // nothing breaks either).
    vec3 n = mat3(push.transform) * inNormal;
    float len = length(n);
    vec3 offset = len > 1e-6 ? (n / len) * push.flags.y : vec3(0.0);

    gl_Position = ubo.proj * ubo.view * vec4(worldPos.xyz + offset, 1.0);
}
