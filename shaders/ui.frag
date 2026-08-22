#version 450

// El atlas de sprites es una imagen SRGB: el sampler ya devuelve lineal y el
// attachment del pase de UI (B8G8R8A8_SRGB) reconvierte al escribir. Por ese
// camino no hay que aplicar ninguna correccion de gamma extra aqui — pero por
// el del pase de ESCENA si, y de eso va pc.linearOutput (ver el final de main).
//
// El atlas de una FUENTE, en cambio, es UNORM: un MSDF son distancias, no
// color, y muestrearlo por una vista SRGB las deforma sin dar ni un aviso de
// validacion. Quien lo declara es UiFont, no este shader.

layout(set = 0, binding = 0) uniform sampler2D uAtlas;

// El MISMO bloque que declara ui.vert, miembro a miembro: el push constant es
// uno solo para las dos etapas. Aqui solo se lee linearOutput, pero la mat4
// tiene que estar declarada delante o el flag caeria en otro offset — y eso no
// da ni error de compilacion ni aviso de validacion, solo un flag con basura.
layout(push_constant) uniform Push {
    mat4 proj;
    int  linearOutput;
} pc;

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

    // El color se calcula en una local y se escribe UNA sola vez al final: la
    // correccion de gamma de abajo tiene que pasar por los tres caminos, y con
    // un `return` por rama se olvidaria en dos de ellos sin que nada avisara.
    vec4 color;

    // Modo 0: exactamente lo de siempre. Un quad de sprite o de color plano
    // sale igual que antes de que existiera el texto, y en el mismo lote.
    if (vParams.x < 0.5)
    {
        // Alpha recto: el blending de fuera hace SRC_ALPHA / ONE_MINUS_SRC_ALPHA,
        // asi que aqui NO se premultiplica el color por el alfa.
        color = tex * vColor;
    }
    else
    {
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
            color = vec4(rgb, alpha);
        }
        else
        {
            color = vec4(vColor.rgb, vColor.a * fill);
        }
    }

    // El pase de UI escribe en un attachment SRGB: el hardware codifica al
    // escribir, asi que ahi este numero YA ES la luz lineal que se ve.
    //
    // El pase de ESCENA no: es HDR LINEAL (kHdrFormat) y todo lo que se escribe
    // ahi pasa despues por bloom_composite.frag (ACES + pow(1/2.2)). Escribir
    // el mismo numero lo saca LAVADO — un 0.5 acaba en ~0.80 en pantalla, no en
    // 0.5. Deshacer aqui la gamma lo devuelve a su sitio (~0.60); lo que queda
    // de diferencia es el tonemap, y ESO es deseable: un cartel que esta en el
    // mundo tiene que exponerse como el resto de la escena. Ninguna capa de
    // validacion dice una palabra de esto, el sintoma es solo el color.
    //
    // El max() es porque pow() con base negativa es comportamiento indefinido.
    if (pc.linearOutput != 0)
        color.rgb = pow(max(color.rgb, vec3(0.0)), vec3(2.2));

    outColor = color;
}
