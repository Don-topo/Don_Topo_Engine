#version 450

// Fullscreen triangle — no VBO, 3 vértices cubren toda la pantalla
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(push_constant) uniform Push {
    mat4 invViewProj;
} push;

layout(location = 0) out vec3 outDir;

void main() {
    vec2 pos      = positions[gl_VertexIndex];
    gl_Position   = vec4(pos, 1.0, 1.0); // z=1 → far plane

    // Reconstruir dirección world-space (view sin traslación pasado por caller)
    vec4 world = push.invViewProj * vec4(pos, 1.0, 1.0);
    outDir     = normalize(world.xyz / world.w);
}
