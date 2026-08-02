#version 450

// TAA: acumulacion temporal. El pass de escena se ha renderizado con un jitter
// de subpixel distinto cada frame (secuencia de Halton), asi que promediar este
// frame con el historial equivale a supersamplear repartido en el tiempo.
//
// NO hay buffer de velocidad en el motor, asi que la reproyeccion es solo por
// profundidad: acierta con la geometria estatica y con la camara en movimiento,
// que es el caso que produce casi todo el escalonado visible. Los objetos que se
// mueven por si mismos (y las mallas skinned) reproyectan a la posicion
// equivocada; lo que impide que eso se convierta en un rastro persistente es el
// recorte al vecindario de abajo, que ata el historial a los colores que de
// verdad hay alrededor del pixel en este frame.
layout(location = 0) in  vec2 inUv;
// Dos destinos a la vez: la imagen que se presenta y el historial que leera el
// frame siguiente. Es el mismo color, escrito una sola vez.
layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outHistory;

layout(set = 0, binding = 0) uniform sampler2D currentTex;
layout(set = 0, binding = 1) uniform sampler2D historyTex;
layout(set = 0, binding = 2) uniform sampler2D depthTex;

layout(push_constant) uniform Push {
    // Clip de ESTE frame (sin jitter) -> clip del anterior. Se calcula en la CPU
    // como prevViewProj * inverse(currViewProj) para no gastar dos mat4.
    mat4  reproject;
    vec2  invRes;
    float feedback;      // peso del historial: 0.9 = 90% historial, 10% frame nuevo
    int   historyValid;  // 0 el primer frame, tras un resize o al cambiar de modo
} push;

void main()
{
    const vec3 current = texture(currentTex, inUv).rgb;

    if (push.historyValid == 0)
    {
        outColor   = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    // Reproyeccion: de (uv, depth) de este frame a la uv que ocupaba ese mismo
    // punto en el frame anterior. El depth sale del pre-pass de profundidad, que
    // se graba SIN jitter, asi que la posicion reconstruida es la geometrica.
    const float depth = texture(depthTex, inUv).r;

    vec4 clip = vec4(inUv * 2.0 - 1.0, depth, 1.0);
    vec4 prev = push.reproject * clip;
    // Fondo lejano (depth == 1.0): la w puede quedar degenerada. Se trata como
    // historial no valido en vez de dividir por casi cero.
    vec2 prevUv = (abs(prev.w) > 1e-6) ? (prev.xy / prev.w) * 0.5 + 0.5 : vec2(-1.0);

    // Fuera de pantalla el frame anterior: no habia nada que acumular ahi.
    if (prevUv.x < 0.0 || prevUv.x > 1.0 || prevUv.y < 0.0 || prevUv.y > 1.0)
    {
        outColor   = vec4(current, 1.0);
        outHistory = vec4(current, 1.0);
        return;
    }

    vec3 history = texture(historyTex, prevUv).rgb;

    // Recorte al vecindario 3x3: el historial se limita a la caja de colores que
    // rodean al pixel EN ESTE FRAME. Es lo que corta el ghosting cuando la
    // reproyeccion falla (objetos moviles, desocclusiones): si el color viejo no
    // se parece a nada de lo que hay ahora alrededor, se recorta hasta que si.
    vec3 boxMin = current;
    vec3 boxMax = current;
    for (int y = -1; y <= 1; y++)
    {
        for (int x = -1; x <= 1; x++)
        {
            if (x == 0 && y == 0) continue;
            vec3 c = texture(currentTex, inUv + vec2(x, y) * push.invRes).rgb;
            boxMin = min(boxMin, c);
            boxMax = max(boxMax, c);
        }
    }
    history = clamp(history, boxMin, boxMax);

    const vec3 result = mix(current, history, push.feedback);

    outColor   = vec4(result, 1.0);
    outHistory = vec4(result, 1.0);
}
