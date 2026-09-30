#version 450

// Triangle that covers the screen without a vertex buffer: three vertices generated from
// gl_VertexIndex (0,1,2). A triangle and not a quad because that way there is no
// interior diagonal where the rasterization quads get duplicated.
//
// uv comes from the SAME coordinates as gl_Position (uv = ndc*0.5+0.5), so
// the texture's texel (0,0) falls on the fragment at NDC y=-1: the top row
// of the framebuffer. It is a 1:1 mapping with the offscreen image, without flipping
// anything — the Y already comes inverted in the scene's projection.
layout(location = 0) out vec2 outUv;

void main()
{
    outUv       = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);
}
