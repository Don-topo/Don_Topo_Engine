#version 450

// Fullscreen triangle — sin VBO, 3 vértices cubren la pantalla. Mismo patrón
// que skybox.vert. Emite UV en [0,1] para muestrear la textura del logo.
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 outUV;

void main() {
    vec2 pos    = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    // pos en [-1,3] -> UV en [0,2] (el triángulo desborda; el recorte a
    // pantalla deja UV en [0,1] visible). UV.y sin voltear: la textura se sube
    // tal cual y el letterbox se calcula en el fragment.
    outUV = pos * 0.5 + 0.5;
}
