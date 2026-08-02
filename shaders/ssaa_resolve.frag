#version 450

// Bajada del SSAA: la escena entera se ha renderizado a m_renderExtent (el
// tamano de la ventana multiplicado por el factor) y aqui se promedia hasta el
// tamano real de m_offscreenImage.
//
// El promedio se hace en LINEAL, no en gamma: la fuente es B8G8R8A8_SRGB y el
// sampler ya devuelve el color decodificado, que es justo lo que hay que
// promediar para que el resultado sea energeticamente correcto. El attachment de
// salida vuelve a codificarlo solo.
layout(location = 0) in  vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Push {
    vec2 invSrc;   // 1/tamano de la imagen GRANDE (la fuente)
    int  taps;     // muestras por eje: 2 para 2x, 3 para 3x...
} push;

void main()
{
    // Con un solo tap esto seria una copia: el sampler lineal ya promediaria
    // cuatro texeles, pero solo los cuatro que rodean al centro exacto, que a
    // factor 2 deja fuera parte de la huella del pixel de destino.
    if (push.taps <= 1)
    {
        outColor = vec4(texture(srcTex, inUv).rgb, 1.0);
        return;
    }

    // Rejilla centrada de taps x taps dentro de la huella del pixel de destino.
    // Los offsets van en texeles de la imagen FUENTE.
    const float n     = float(push.taps);
    const float start = -0.5 * (n - 1.0);

    vec3 sum = vec3(0.0);
    for (int y = 0; y < push.taps; y++)
    {
        for (int x = 0; x < push.taps; x++)
        {
            vec2 off = (vec2(start + float(x), start + float(y))) * push.invSrc;
            sum += texture(srcTex, inUv + off).rgb;
        }
    }

    outColor = vec4(sum / (n * n), 1.0);
}
