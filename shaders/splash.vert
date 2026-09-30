#version 450

// Fullscreen triangle — no VBO, 3 vertices cover the screen. Same pattern
// as skybox.vert. Emits UV in [0,1] to sample the logo texture.
const vec2 positions[3] = vec2[](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 outUV;

void main() {
    vec2 pos    = positions[gl_VertexIndex];
    gl_Position = vec4(pos, 0.0, 1.0);
    // pos in [-1,3] -> UV in [0,2] (the triangle overflows; clipping to the
    // screen leaves UV in [0,1] visible). UV.y not flipped: the texture is uploaded
    // as is and the letterbox is computed in the fragment.
    outUV = pos * 0.5 + 0.5;
}
