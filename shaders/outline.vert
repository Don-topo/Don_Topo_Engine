#version 450

// Casco invertido del objeto seleccionado en el editor: la misma malla
// extruida a lo largo de su normal, dibujada con las caras frontales
// culleadas. Solo asoma el reborde que queda fuera de la silueta original;
// el resto del casco cae por detras de la geometria y lo descarta el depth
// test. Comparte pipeline layout con triangle.vert (mismo UBO en set 0 y el
// mismo bloque de push constants), asi que no necesita descriptor sets ni
// rangos propios.

layout(location = 0) in vec3 inPos;
// Las locations 1 (color), 2 (uv) y 4 (tangent) del vertex input existen en
// el pipeline pero no se declaran aqui: un shader no esta obligado a
// consumir todos los atributos del binding.
layout(location = 3) in vec3 inNormal;

// El array de cascadas no se usa aqui, pero se declara igual que en los demas
// shaders del set 0: si el bloque se quedara con una sola mat4, cualquier
// miembro que se anadiera detras leeria de un offset distinto al que escribe
// el UBO de C++.
#define SHADOW_CASCADES 4
// Huecos de matriz de sombra: 4 cascadas de una direccional o 6 caras del
// cubemap de una de punto. Nunca coexisten. Mismo valor que
// SHADOW_MATRICES en UniformBufferObject.h.
#define SHADOW_MATRICES 6

layout(set = 0, binding = 0) uniform UBO
{
    mat4 view;
    mat4 proj;
    mat4 lightSpaceMatrix[SHADOW_MATRICES];
} ubo;

// Mismo layout que el bloque de triangle.vert/pbr.frag: mat4 + 2 float + vec2.
// flags.x no se usa aqui (el outline dibuja UN objeto con su matriz en el push,
// nunca por instancing); flags.y lleva el grosor de la extrusion en unidades de
// mundo, que era el hueco libre de esa vec2.
layout(push_constant) uniform PushData
{
    mat4  transform;
    float metallic;
    float roughness;
    vec2  flags;      // y: grosor del contorno (mundo)
} push;

void main()
{
    vec4 worldPos = push.transform * vec4(inPos, 1.0);

    // Normal a mundo SIN normalizar primero: mat3(transform) puede llevar
    // escala. Si sale de longitud cero (malla sin normales) normalize daria
    // NaN y el triangulo entero desapareceria, asi que en ese caso no se
    // extruye y el casco coincide con la malla (no se ve contorno, pero
    // tampoco se rompe nada).
    vec3 n = mat3(push.transform) * inNormal;
    float len = length(n);
    vec3 offset = len > 1e-6 ? (n / len) * push.flags.y : vec3(0.0);

    gl_Position = ubo.proj * ubo.view * vec4(worldPos.xyz + offset, 1.0);
}
