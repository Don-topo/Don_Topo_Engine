#version 450

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUV;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec3 inTangent;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragUV; 
layout(location = 2) out vec3 fragNormal;
layout(location = 3) out vec3 fragWorldPos;
layout(location = 4) out vec3 fragTangent;
layout(location = 5) out vec3 fragBitangent;
// No hay varying de posicion en espacio de luz: con cascadas harian falta N, y
// el fragment shader ya reconstruye la que toca desde fragWorldPos.

#define SHADOW_CASCADES 4
// Huecos de matriz de sombra. Los 6 primeros son de la luz KEY (4 cascadas,
// o 6 caras de cubemap, o 1 cara de foco); los 4 de detras son un foco
// secundario cada uno. Mismo valor que SHADOW_MATRICES en
// UniformBufferObject.h.
#define SHADOW_KEY_MATRICES 6
#define SHADOW_MATRICES 10

layout(set = 0, binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix[SHADOW_MATRICES];
} ubo;

// Transforms por instancia, uno por frame-in-flight. Los objetos estaticos que
// comparten malla+material se dibujan en un solo draw instanciado y cada
// instancia coge su matriz de aqui por gl_InstanceIndex.
layout(std430, set = 1, binding = 0) readonly buffer InstanceData
{
    mat4 models[];
} instances;

// Mismos tipos y offsets que el bloque de pbr.frag (las dos etapas del mismo
// pipeline comparten el rango de push constants): mat4 + 2 float + vec2. El
// hueco de esa vec2 era relleno; ahora su .x lleva el flag de instancing y su .y
// sigue sin usarse, asi que ningun offset se ha movido y pbr.frag no cambia.
layout(push_constant) uniform PushData
{
    mat4  transform;
    float metallic;
    float roughness;
    vec2  flags;      // x: 1 = coger el model matrix del SSBO de instancias
} push;

void main()
{
    // useInstancing == 0 es la ruta skinned: comparte este vertex shader y este
    // pipeline layout, dibuja una sola instancia y trae su matriz en el push
    // constant, no en el SSBO.
    mat4 model = push.flags.x != 0.0 ? instances.models[gl_InstanceIndex] : push.transform;

    gl_Position = ubo.proj * ubo.view * model * vec4(inPos, 1.0);
    fragColor   = inColor;
    fragUV      = inUV;
    fragNormal   = mat3(model) * inNormal;
    fragWorldPos = vec3(model * vec4(inPos, 1.0));
    vec3 T = normalize(mat3(model) * inTangent);
    vec3 N = normalize(mat3(model) * inNormal);
    T = normalize(T - dot(T, N) * N);
    fragTangent   = T;
    fragBitangent = cross(N, T);
}