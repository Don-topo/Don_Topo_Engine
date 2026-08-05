#version 450

// Quads 2D de la UI. Las posiciones llegan en PIXELES con (0,0) arriba a la
// izquierda; la ortografica del push constant (orthoRH_ZO con top=0 y
// bottom=alto) es quien las lleva a NDC sin voltear nada aqui.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(push_constant) uniform Push {
    mat4 proj;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;

void main()
{
    gl_Position = pc.proj * vec4(inPos, 0.0, 1.0);
    vUv    = inUv;
    vColor = inColor;
}
