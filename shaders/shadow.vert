#version 450

layout(location = 0) in vec3 inPos;

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix;
} ubo;

// Mismo SSBO por frame que triangle.vert (set 1, binding 0), pero con su propio
// rango: el pass de sombras culea con el frustum de la LUZ, asi que el conjunto
// visible no es el de la camara y sus transforms van en otro tramo del buffer.
// Este pass solo dibuja objetos estaticos agrupados, asi que no hay ruta de push
// constant que conservar.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

void main()
{
    gl_Position = ubo.lightSpaceMatrix * instances.models[gl_InstanceIndex] * vec4(inPos, 1.0);
}