#version 450

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
} ubo;

layout(push_constant) uniform Push {
    mat4 transform;
} push;

void main()
{
    gl_Position = ubo.lightSpaceMatrix * push.transform * vec4(inPos, 1.0);
}