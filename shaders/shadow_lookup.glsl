// Coordenada de muestreo del shadow map de la luz key.
//
// Lo incluyen pbr.frag y fog.comp, que muestrean EL MISMO mapa. Antes cada uno
// tenia su copia de la seleccion de cascada y de la reproyeccion; cambiar una y
// no la otra dejo el in-scattering de la niebla apuntando a un lado y el shadow
// map construido hacia otro (H65), y ese fallo no lo delata ninguna capa de
// validacion. Por eso lo que tiene que ser identico vive aqui.
//
// Lo que NO vive aqui es el filtrado: la escena hace PCF 3x3 y la niebla un solo
// tap, porque la niebla ya promedia a lo largo de la marcha y multiplicar por
// nueve sus taps se paga en cada paso de cada rayo. Esa diferencia es
// deliberada; la eleccion de capa y la reproyeccion, no.
//
// INCLUSION TEXTUAL: va DESPUES de declarar `ubo` (con view, lightSpaceMatrix[]
// y cascadeSplits) y el sampler `shadowMap`, porque estas funciones los usan por
// nombre. Y despues de #define SHADOW_CASCADES.

// Tipo de la luz key, con la misma convencion que el resto del motor:
// direction.w lleva el tipo y se lee con int(w + 0.5). SOLO la luz 0 proyecta
// sombra.
//   0 point, 1 spot, 2 directional, 3 area.
int dtKeyLightType()
{
    return int(ubo.lights[0].direction.w + 0.5);
}

// Cascada que le toca a un fragmento por su profundidad en view space. Los
// cortes vienen ya ordenados; el ultimo es el alcance total de las sombras.
// -1 = mas alla de ese alcance, no hay mapa que muestrear.
int dtSelectCascade(float viewDepth)
{
    for (int i = 0; i < SHADOW_CASCADES; i++)
        if (viewDepth <= ubo.cascadeSplits[i]) return i;
    return -1;
}

// Donde muestrear el shadow map para un punto del mundo.
//
//   uvz   = (u, v, profundidad de referencia) ya en [0,1]
//   layer = capa del array
//
// false = este punto no lo cubre el mapa y el llamante debe tratarlo como
// ILUMINADO (1.0). Pasa mas alla del alcance de las cascadas, y fuera del cono
// de un foco.
//
// Dos caminos, segun el tipo de la luz key:
//
//  - Direccional: sombras en CASCADA. Proyeccion ortografica, o sea sombra
//    paralela, que es lo correcto para una luz que esta en el infinito. La
//    cascada se elige por profundidad de camara.
//
//  - Foco: UNA cara en PERSPECTIVA desde la posicion de la luz, en la capa 0.
//    Su matriz va en lightSpaceMatrix[0] —las cascadas y esto no coexisten
//    nunca, porque solo hay una luz key y solo tiene un tipo—, y la perspectiva
//    es lo que hace que su sombra DIVERJA: crece al alejarse de la luz, en vez
//    de mantener el tamano como hacia la aproximacion en cascada.
//
//  - Punto, y foco de mas de 90 grados: CUBEMAP de seis caras, una por semieje,
//    en las capas 0..5. Cada una es una perspectiva de 90 grados desde la luz,
//    asi que la sombra diverge en todas las direcciones y ninguna cara reparte
//    sus texeles sobre mas de un octante. Es el unico camino que usa mas de
//    cuatro huecos de lightSpaceMatrix, y por eso ese array tiene
//    SHADOW_MATRICES = 6.

// Bias en ESPACIO DE MUNDO para los dos caminos en PERSPECTIVA (foco y punto).
// El del rasterizador esta afinado para la ortografica de las cascadas, donde la
// profundidad NDC es lineal en la distancia real; en perspectiva no lo es, y
// corregirlo por ahi obligaria a un par de PSO extra en CADA backend porque el
// bias es estado de pipeline en los dos.
//
// Dos terminos, que atacan cosas distintas:
//
//  - NORMAL-OFFSET proporcional a la distancia a la luz. En perspectiva la
//    huella de un texel CRECE con esa distancia, y el error de cuantizacion con
//    ella; un offset fijo que vale cerca de la bombilla se queda corto lejos.
//    Separarse por la normal es lo que saca a la superficie de su propia sombra,
//    porque el error esta en el plano de la superficie.
//
//  - Empuje HACIA LA LUZ, pequeno y constante, para la superficie vista casi de
//    canto desde la luz, donde la normal apenas separa en profundidad.
//
// normalMundo == vec3(0) -> punto en el AIRE (la marcha de la niebla): no puede
// auto-sombrearse, asi que solo se aplica el empuje.
vec3 dtBiasHaciaLaLuz(int luz, vec3 worldPos, vec3 normalMundo)
{
    vec3  aLaLuz  = ubo.lights[luz].position.xyz - worldPos;
    float distLuz = length(aLaLuz);
    if (distLuz <= 1e-5) return worldPos;
    return worldPos + (aLaLuz / distLuz) * 0.02 + normalMundo * (distLuz * 0.004);
}

// normalMundo = normal GEOMETRICA de la superficie, para el bias de los caminos
// en perspectiva. Un punto que no esta sobre ninguna superficie —la marcha de la
// niebla— pasa vec3(0.0).
// Una sola cara en perspectiva, en la ranura dada. La usan el foco key (ranura
// 0) y cada foco secundario (ranuras SHADOW_KEY_MATRICES en adelante), asi que
// el recorte del cono y el bias viven en un solo sitio.
bool dtCaraPerspectiva(int luz, int ranura, vec3 worldPos, vec3 normalMundo,
                       out vec3 uvz, out float layer)
{
    worldPos = dtBiasHaciaLaLuz(luz, worldPos, normalMundo);

    vec4 ls = ubo.lightSpaceMatrix[ranura] * vec4(worldPos, 1.0);
    // Detras de la luz: w <= 0 hace que la division devuelva el punto reflejado,
    // que caeria dentro del mapa y pintaria una sombra fantasma al otro lado.
    if (ls.w <= 0.0) return false;

    vec3 p = ls.xyz / ls.w;
    p.xy   = p.xy * 0.5 + 0.5;
    // Fuera del frustum del foco no hay nada grabado. El recorte en xy es
    // obligatorio aqui y no en las cascadas: el volumen de una cascada se ajusta
    // a lo que se ve, mientras que el cono de un foco deja fuera casi toda la
    // escena, y sin esto el sampler estira el borde del mapa por todo el resto
    // del mundo.
    if (p.z < 0.0 || p.z > 1.0) return false;
    if (any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0)))) return false;

    uvz   = p;
    layer = float(ranura);
    return true;
}

// Elige cara de un cubemap por el eje MAYOR de (fragmento - luz): es la que
// mira ese semiespacio, y su frustum de 90 grados contiene el punto.
//
// Solo elige CUAL. La UV y la profundidad salen despues de la matriz de esa
// cara, la misma con la que se grabo — derivarlas a mano obligaria a que dos
// convenciones de cubemap coincidieran, y aqui eso ya salio mal una vez.
int dtCaraDelCubemap(vec3 worldPos, vec3 posLuz)
{
    vec3 L = worldPos - posLuz;
    vec3 a = abs(L);
    if (a.x >= a.y && a.x >= a.z) return L.x > 0.0 ? 0 : 1;
    if (a.y >= a.z)               return L.y > 0.0 ? 2 : 3;
    return L.z > 0.0 ? 4 : 5;
}

// Muestreo en perspectiva desde una ranura ya elegida, sin recortar en xy: lo
// usan las caras de un cubemap, donde salirse por un lado es normal —el punto
// pertenece a la cara vecina— y recortarlo dejaria un corte duro en la diagonal.
bool dtCaraDeCubemap(int luz, int ranura, vec3 worldPos, vec3 normalMundo,
                     out vec3 uvz, out float layer)
{
    worldPos = dtBiasHaciaLaLuz(luz, worldPos, normalMundo);

    vec4 ls = ubo.lightSpaceMatrix[ranura] * vec4(worldPos, 1.0);
    if (ls.w <= 0.0) return false;

    vec3 p = ls.xyz / ls.w;
    p.xy   = p.xy * 0.5 + 0.5;
    // Fuera del alcance de la luz no hay nada grabado, y mas alla del far
    // tampoco ilumina: "sin sombra" es la respuesta correcta.
    if (p.z < 0.0 || p.z > 1.0) return false;

    uvz   = p;
    layer = float(ranura);
    return true;
}

// Sombra de una luz que NO es la key, en la ranura que le dio el reparto.
//
// position.w codifica las dos cosas en un solo campo, porque en el bloque no
// queda otro libre: |w| - 1 es la ranura, y el SIGNO dice por que camino se
// grabo — positivo una sola cara (foco estrecho), negativo las seis de un
// cubemap (luz de punto, o foco tan abierto que una cara le queda mal). Lo pone
// el renderer a partir de lo que el pase HIZO; el shader no deduce el camino
// del tipo de luz, que seria una segunda copia del criterio.
//
// false = esta luz no tiene sombra aqui y el llamante la trata como ILUMINADA.
bool dtShadowCoordExtra(int luz, vec3 worldPos, vec3 normalMundo,
                        out vec3 uvz, out float layer)
{
    uvz   = vec3(0.0);
    layer = 0.0;

    float codigo = ubo.lights[luz].position.w;
    if (abs(codigo) < 0.5) return false;          // no proyecta

    int  ranura  = int(abs(codigo) + 0.5) - 1;
    bool cubemap = codigo < 0.0;
    if (ranura < SHADOW_KEY_MATRICES || ranura >= SHADOW_MATRICES) return false;

    if (!cubemap)
        return dtCaraPerspectiva(luz, ranura, worldPos, normalMundo, uvz, layer);

    // Las seis caras estan en ranuras CONSECUTIVAS desde la que dio el reparto,
    // en el mismo orden que las grabo pointShadowMatrices.
    int cara = dtCaraDelCubemap(dtBiasHaciaLaLuz(luz, worldPos, normalMundo),
                                ubo.lights[luz].position.xyz);
    if (ranura + cara >= SHADOW_MATRICES) return false;
    return dtCaraDeCubemap(luz, ranura + cara, worldPos, normalMundo, uvz, layer);
}

bool dtShadowCoord(vec3 worldPos, vec3 normalMundo, out vec3 uvz, out float layer)
{
    uvz   = vec3(0.0);
    layer = 0.0;

    int tipo = dtKeyLightType();

    // position.w de la luz key = 1 cuando su sombra se grabo como CUBEMAP. Lo
    // pone el renderer a partir de cuantas capas dejo validas el pase, y NO se
    // deduce aqui del tipo: una luz de punto siempre va por cubemap, pero un
    // FOCO muy abierto tambien —por encima de 90 grados de cono una sola cara
    // reparte los texeles sobre tanto mundo que el borde sale escalonado, y
    // empeora con tan(FOV/2)—. Recalcular ese umbral aqui seria una segunda
    // copia del criterio, que es justo lo que rompio H65.
    if (ubo.lights[0].position.w > 0.5)   // cubemap de seis caras
    {
        worldPos = dtBiasHaciaLaLuz(0, worldPos, normalMundo);

        // La cara la decide el eje MAYOR de (fragmento - luz): es la que mira
        // ese semiespacio, y su frustum de 90 grados contiene el punto. Solo se
        // elige CUAL; la UV y la profundidad salen de la matriz de esa cara, la
        // misma con la que se grabo. Derivarlas a mano obligaria a que dos
        // convenciones de cubemap coincidieran, y aqui eso ya salio mal.
        vec3  L = worldPos - ubo.lights[0].position.xyz;
        vec3  a = abs(L);
        int   cara;
        if (a.x >= a.y && a.x >= a.z)      cara = L.x > 0.0 ? 0 : 1;
        else if (a.y >= a.z)               cara = L.y > 0.0 ? 2 : 3;
        else                               cara = L.z > 0.0 ? 4 : 5;

        vec4 ls = ubo.lightSpaceMatrix[cara] * vec4(worldPos, 1.0);
        if (ls.w <= 0.0) return false;

        vec3 p = ls.xyz / ls.w;
        p.xy   = p.xy * 0.5 + 0.5;
        // Fuera del alcance de la luz no hay nada grabado: mas alla del far de
        // la proyeccion tampoco ilumina, asi que "sin sombra" es correcto.
        if (p.z < 0.0 || p.z > 1.0) return false;

        uvz   = p;
        layer = float(cara);
        return true;
    }

    if (tipo == 1)   // foco key: una cara, en la ranura 0
    {
        // Se nota al MOVERSE mas que quieto porque el TAA acumula historia
        // mientras la camara esta parada y la rechaza en cuanto se mueve: sin
        // bias suficiente, el patron de acne cambia con el jitter subpixel y
        // el TAA ya no lo puede promediar.
        return dtCaraPerspectiva(0, 0, worldPos, normalMundo, uvz, layer);
    }

    // Direccional y punto: cascadas.
    float viewDepth = -(ubo.view * vec4(worldPos, 1.0)).z;
    int   cascade   = dtSelectCascade(viewDepth);
    if (cascade < 0) return false;

    vec4 ls = ubo.lightSpaceMatrix[cascade] * vec4(worldPos, 1.0);
    vec3 p  = ls.xyz / ls.w;
    p.xy    = p.xy * 0.5 + 0.5;
    if (p.z > 1.0 || p.z < 0.0) return false;

    uvz   = p;
    layer = float(cascade);
    return true;
}
