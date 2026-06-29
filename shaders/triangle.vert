#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV; 
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;

layout(set = 0, binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    vec4 lightPos;
    vec4 viewPos;
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
    fragNormal   = mat3(push.transform) * inNormal;
    fragWorldPos = vec3(push.transform * vec4(inPos, 1.0));
}