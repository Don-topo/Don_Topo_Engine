#version 450

// FXAA 3.11 (preset de calidad de PC, 12 pasos de busqueda). Corre DESPUES de la
// composicion, sobre la imagen LDR ya tonemapeada: es un filtro de post que
// necesita el color final, no el HDR lineal.
//
// Entra m_fxaaSrcImage (lo que antes escribia la composicion directamente en
// m_offscreenImage, contorno de seleccion y gizmos incluidos) y sale
// m_offscreenImage, que es la que muestrea la UI del editor y la que blitea el
// runtime headless. Con el efecto apagado este pass NO se graba y la composicion
// vuelve a escribir directamente en m_offscreenImage: imagen identica byte a byte.
layout(location = 0) in  vec2 inUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D ldrTex;

layout(push_constant) uniform Push {
    vec2  invRes;             // 1/ancho, 1/alto del viewport
    float subpix;             // fuerza del filtro de subpixel (0 = solo bordes)
    float edgeThreshold;      // contraste relativo minimo para tratar algo como borde
    float edgeThresholdMin;   // contraste absoluto minimo (corta el ruido en zonas oscuras)
} push;

// Luma perceptual. m_offscreenImage es B8G8R8A8_SRGB, asi que el sampler ya
// devuelve el color DECODIFICADO a lineal: se aproxima la vuelta a gamma con
// sqrt antes de pesar los canales. FXAA detecta bordes sobre luma no lineal; si
// se mide en lineal, los bordes de la mitad oscura de la imagen caen por debajo
// del umbral y no se suavizan.
float luma(vec3 c) { return dot(sqrt(c), vec3(0.299, 0.587, 0.114));  }

// Longitud de cada salto de la busqueda de extremos del borde. Los primeros
// pasos son de un texel (precision cerca del pixel) y los ultimos se alargan
// para alcanzar bordes muy tendidos sin gastar iteraciones.
const float kStep[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

void main()
{
    const vec2 rcp = push.invRes;

    const vec3  rgbM  = texture(ldrTex, inUv).rgb;
    const float lumaM = luma(rgbM);

    // Cruz de vecinos: decide si aqui hay borde y con que contraste.
    const float lumaN = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 0, -1)).rgb);
    const float lumaS = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 0,  1)).rgb);
    const float lumaW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1,  0)).rgb);
    const float lumaE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1,  0)).rgb);

    const float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    const float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    const float range   = lumaMax - lumaMin;

    // Zona plana: se devuelve el pixel intacto. Es lo que hace que el interior de
    // las superficies salga exactamente igual que sin FXAA.
    if (range < max(push.edgeThresholdMin, lumaMax * push.edgeThreshold))
    {
        outColor = vec4(rgbM, 1.0);
        return;
    }

    // Diagonales: hacen falta para decidir la ORIENTACION del borde.
    const float lumaNW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1, -1)).rgb);
    const float lumaNE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1, -1)).rgb);
    const float lumaSW = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2(-1,  1)).rgb);
    const float lumaSE = luma(textureLodOffset(ldrTex, inUv, 0.0, ivec2( 1,  1)).rgb);

    const float lumaNS = lumaN + lumaS;
    const float lumaWE = lumaW + lumaE;

    // Segunda derivada en cada eje: gana el eje en el que el color cambia MENOS,
    // que es a lo largo del que corre el borde.
    const float edgeHorz = abs(-2.0 * lumaW + lumaNW + lumaSW)
                         + abs(-2.0 * lumaM + lumaNS) * 2.0
                         + abs(-2.0 * lumaE + lumaNE + lumaSE);
    const float edgeVert = abs(-2.0 * lumaN + lumaNW + lumaNE)
                         + abs(-2.0 * lumaM + lumaWE) * 2.0
                         + abs(-2.0 * lumaS + lumaSW + lumaSE);

    const bool horzSpan = edgeHorz >= edgeVert;

    // Los dos vecinos perpendiculares al borde y su gradiente.
    float luma1 = horzSpan ? lumaN : lumaW;
    float luma2 = horzSpan ? lumaS : lumaE;
    float grad1 = luma1 - lumaM;
    float grad2 = luma2 - lumaM;

    // De los dos lados, el del salto mas fuerte es el borde "de verdad".
    const bool  steepest    = abs(grad1) >= abs(grad2);
    const float gradScaled  = 0.25 * max(abs(grad1), abs(grad2));

    // Se avanza medio texel hacia el borde para muestrear justo encima de el.
    float stepLength = horzSpan ? rcp.y : rcp.x;
    float lumaLocal  = 0.0;
    if (steepest)
    {
        stepLength = -stepLength;
        lumaLocal  = 0.5 * (luma1 + lumaM);
    }
    else
    {
        lumaLocal  = 0.5 * (luma2 + lumaM);
    }

    vec2 currentUv = inUv;
    if (horzSpan) currentUv.y += stepLength * 0.5;
    else          currentUv.x += stepLength * 0.5;

    // Recorrido a lo largo del borde en las dos direcciones hasta encontrar sus
    // extremos: es lo que distingue a FXAA de un simple desenfoque de 3x3.
    const vec2 offset = horzSpan ? vec2(rcp.x, 0.0) : vec2(0.0, rcp.y);
    vec2 uv1 = currentUv - offset * kStep[0];
    vec2 uv2 = currentUv + offset * kStep[0];

    float lumaEnd1 = luma(texture(ldrTex, uv1).rgb) - lumaLocal;
    float lumaEnd2 = luma(texture(ldrTex, uv2).rgb) - lumaLocal;

    bool reached1 = abs(lumaEnd1) >= gradScaled;
    bool reached2 = abs(lumaEnd2) >= gradScaled;

    if (!reached1) uv1 -= offset * kStep[1];
    if (!reached2) uv2 += offset * kStep[1];

    if (!reached1 || !reached2)
    {
        for (int i = 2; i < 12; i++)
        {
            if (!reached1) lumaEnd1 = luma(texture(ldrTex, uv1).rgb) - lumaLocal;
            if (!reached2) lumaEnd2 = luma(texture(ldrTex, uv2).rgb) - lumaLocal;

            reached1 = reached1 || abs(lumaEnd1) >= gradScaled;
            reached2 = reached2 || abs(lumaEnd2) >= gradScaled;

            if (reached1 && reached2) break;

            if (!reached1) uv1 -= offset * kStep[i];
            if (!reached2) uv2 += offset * kStep[i];
        }
    }

    // Distancia a cada extremo: cuanto mas cerca esta el pixel de un extremo,
    // menos hay que desplazarlo.
    const float dist1 = horzSpan ? (inUv.x - uv1.x) : (inUv.y - uv1.y);
    const float dist2 = horzSpan ? (uv2.x - inUv.x) : (uv2.y - inUv.y);

    const bool  nearest1  = dist1 < dist2;
    const float distFinal = min(dist1, dist2);
    const float spanLen   = dist1 + dist2;

    float pixelOffset = -distFinal / spanLen + 0.5;

    // Si el extremo mas cercano cambia de signo respecto al pixel, el borde no
    // pasa por aqui y desplazar seria emborronar de mas.
    const bool  lumaMSmaller = lumaM < lumaLocal;
    const bool  goodSpan     = ((nearest1 ? lumaEnd1 : lumaEnd2) < 0.0) != lumaMSmaller;
    float finalOffset = goodSpan ? pixelOffset : 0.0;

    // Filtro de subpixel: media de los 3x3 contra el pixel, para los detalles mas
    // finos que un borde completo (cables, rejillas, el contorno de seleccion).
    const float lumaAvg   = (1.0 / 12.0) * (2.0 * (lumaNS + lumaWE) + lumaNW + lumaNE + lumaSW + lumaSE);
    const float subDelta  = clamp(abs(lumaAvg - lumaM) / range, 0.0, 1.0);
    const float subSmooth = (-2.0 * subDelta + 3.0) * subDelta * subDelta;
    const float subOffset = subSmooth * subSmooth * push.subpix;

    finalOffset = max(finalOffset, subOffset);

    vec2 finalUv = inUv;
    if (horzSpan) finalUv.y += finalOffset * stepLength;
    else          finalUv.x += finalOffset * stepLength;

    // El alfa se fuerza a 1.0 igual que en la composicion: es lo que espera la UI
    // del editor al muestrear esta imagen y el blit al swapchain.
    outColor = vec4(texture(ldrTex, finalUv).rgb, 1.0);
}
