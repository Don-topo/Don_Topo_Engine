#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushData
{
    mat4 transform;
} push;

void main()
{
    gl_Position = push.transform * vec4(inPos, 0.0, 1.0);
    fragColor = inColor;
}