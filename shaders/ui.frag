#version 450

// El atlas es una imagen SRGB: el sampler ya devuelve lineal y el attachment de
// composicion (B8G8R8A8_SRGB) reconvierte al escribir. No hay que aplicar
// ninguna correccion de gamma extra aqui.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
    // Alpha recto: el blending de fuera hace SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
    // asi que aqui NO se premultiplica el color por el alfa.
    outColor = texture(uAtlas, vUv) * vColor;
}
