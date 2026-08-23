#version 450

// Quads 2D de la UI. Las posiciones llegan en PIXELES con (0,0) arriba a la
// izquierda; la ortografica del push constant (orthoRH_ZO con top=0 y
// bottom=alto) es quien las lleva a NDC sin voltear nada aqui.

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
// params.x = modo (0 = sprite/color plano, 1 = MSDF)
// params.y = screenPxRange ya escalado al tamano de ESTE quad
// params.z = grosor del outline en pixeles de pantalla
// effect   = color del outline. Todo por vertice: asi el texto no parte el lote.
layout(location = 3) in vec4 inParams;
layout(location = 4) in vec4 inEffect;

layout(push_constant) uniform Push {
    mat4 proj;
    // 0 = el destino es SRGB y el hardware convierte al escribir; 1 = el
    // destino es HDR LINEAL (el pase de escena) y la conversion la hace a mano
    // ui.frag. Aqui no se lee, pero el bloque tiene que ir DECLARADO IGUAL en
    // las dos etapas: el push constant es UNO SOLO para vertex y fragment, y un
    // desajuste de offsets entre ellas no da ni error ni aviso de validacion.
    int linearOutput;
} pc;

layout(location = 0) out vec2 vUv;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vParams;
layout(location = 3) out vec4 vEffect;

void main()
{
    gl_Position = pc.proj * vec4(inPos, 0.0, 1.0);
    vUv     = inUv;
    vColor  = inColor;
    vParams = inParams;
    vEffect = inEffect;
}
