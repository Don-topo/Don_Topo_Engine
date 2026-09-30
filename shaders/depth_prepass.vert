#version 450

layout(location = 0) in vec3 inPos;

// Only the first two members of the block: std140 puts them at offsets 0 and
// 64 no matter what comes behind, so declaring the trimmed block is valid and
// avoids repeating the rest of the UBO here.
layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

// Same per-frame SSBO as triangle.vert and shadow.vert (set 1, binding 0), with
// its own range: this pass culls with the CAMERA's frustum, so its
// section of the buffer goes behind the cascades'.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

void main()
{
    gl_Position = ubo.proj * ubo.view * instances.models[gl_InstanceIndex] * vec4(inPos, 1.0);
}
