#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
} push;

void main() {
    gl_Position = push.viewProj * vec4(inPos, 1.0);
    fragColor = inColor;
}
