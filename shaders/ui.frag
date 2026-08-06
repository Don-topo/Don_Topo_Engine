#version 450

// El atlas de sprites es una imagen SRGB: el sampler ya devuelve lineal y el
// attachment de composicion (B8G8R8A8_SRGB) reconvierte al escribir. No hay que
// aplicar ninguna correccion de gamma extra aqui.
//
// El atlas de una FUENTE, en cambio, es UNORM: un MSDF son distancias, no
// color, y muestrearlo por una vista SRGB las deforma sin dar ni un aviso de
// validacion. Quien lo declara es UiFont, no este shader.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vParams;
layout(location = 3) in vec4 vEffect;

layout(location = 0) out vec4 outColor;

// La mediana de los tres canales es la distancia con signo reconstruida: es lo
// que hace que las esquinas sigan siendo esquinas al ampliar.
float median3(vec3 c)
{
    return max(min(c.r, c.g), min(max(c.r, c.g), c.b));
}

void main()
{
    vec4 tex = texture(uAtlas, vUv);

    // Modo 0: exactamente lo de siempre. Un quad de sprite o de color plano
    // sale igual que antes de que existiera el texto, y en el mismo lote.
    if (vParams.x < 0.5)
    {
        // Alpha recto: el blending de fuera hace SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
        // asi que aqui NO se premultiplica el color por el alfa.
        outColor = tex * vColor;
        return;
    }

    // Distancia en PIXELES DE PANTALLA: 0.5 es el borde y screenPxRange convierte
    // el rango normalizado del MSDF al tamano al que se esta dibujando el quad.
    float px = vParams.y * (median3(tex.rgb) - 0.5);

    float fill = clamp(px + 0.5, 0.0, 1.0);

    if (vParams.z > 0.0)
    {
        // El outline es la MISMA distancia desplazada: ni segunda textura ni
        // rehornear nada.
        float outer = clamp(px + vParams.z + 0.5, 0.0, 1.0);
        vec3  rgb   = mix(vEffect.rgb, vColor.rgb, fill);
        float alpha = mix(vEffect.a * outer, vColor.a, fill);
        outColor = vec4(rgb, alpha);
        return;
    }

    outColor = vec4(vColor.rgb, vColor.a * fill);
}
