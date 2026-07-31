#version 450

// Triangulo que cubre la pantalla sin vertex buffer: tres vertices generados a
// partir de gl_VertexIndex (0,1,2). Un triangulo y no un quad porque asi no hay
// diagonal interior donde se dupliquen los quads de rasterizacion.
//
// uv sale de las MISMAS coordenadas que gl_Position (uv = ndc*0.5+0.5), asi que
// el texel (0,0) de la textura cae en el fragmento de NDC y=-1: la fila de
// arriba del framebuffer. Es un mapeo 1:1 con la imagen offscreen, sin voltear
// nada — la Y ya viene invertida en la proyeccion de la escena.
layout(location = 0) out vec2 outUv;

void main()
{
    outUv       = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(outUv * 2.0 - 1.0, 0.0, 1.0);
}
