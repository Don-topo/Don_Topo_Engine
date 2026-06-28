#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV; 

layout(set = 0, binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform PushData
{
    mat4 transform;
} push;

void main()
{
    gl_Position = ubo.proj * ubo.view * push.transform * vec4(inPos, 1.0);
    fragColor   = inColor;
    fragUV      = inUV;
}