#version 450

layout(location = 0) in vec3 inPos;

// Solo los dos primeros miembros del bloque: std140 los deja en los offsets 0 y
// 64 pase lo que pase detras, asi que declarar el bloque recortado es valido y
// evita repetir aqui el resto del UBO.
layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

// Mismo SSBO por frame que triangle.vert y shadow.vert (set 1, binding 0), con
// su propio rango: este pass culea con el frustum de la CAMARA, asi que su
// tramo del buffer va detras del de las cascadas.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

void main()
{
    gl_Position = ubo.proj * ubo.view * instances.models[gl_InstanceIndex] * vec4(inPos, 1.0);
}
