#version 450

layout(location = 0) in vec3 inPos;

#define SHADOW_CASCADES 4
// Huecos de matriz de sombra. Los 6 primeros son de la luz KEY (4 cascadas,
// o 6 caras de cubemap, o 1 cara de foco); los 4 de detras son un foco
// secundario cada uno. Mismo valor que SHADOW_MATRICES en
// UniformBufferObject.h.
#define SHADOW_KEY_MATRICES 6
#define SHADOW_MATRICES 10

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix[SHADOW_MATRICES];
} ubo;

// Que capa del texture array se esta grabando. Rango propio del pipeline de
// sombras: no comparte pipeline layout con triangle/pbr/outline, asi que el
// bloque PushData de esos no se toca.
layout(push_constant) uniform ShadowPush {
    uint cascade;
} push;

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
    gl_Position = ubo.lightSpaceMatrix[push.cascade] * instances.models[gl_InstanceIndex] * vec4(inPos, 1.0);
}