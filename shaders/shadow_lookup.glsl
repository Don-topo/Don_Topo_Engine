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
// Una luz de PUNTO todavia cae en el camino de cascadas: no tiene direccion
// propia, se le aproxima una y su sombra sigue sin diverger. Eso es P21 etapa 2
// (cubemap de 6 caras) en docs/renderer-audit.md.
// normalMundo = normal GEOMETRICA de la superficie, para el bias del foco. Un
// punto que no esta sobre ninguna superficie —la marcha de la niebla— pasa
// vec3(0.0) y se queda solo con el empuje hacia la luz.
bool dtShadowCoord(vec3 worldPos, vec3 normalMundo, out vec3 uvz, out float layer)
{
    uvz   = vec3(0.0);
    layer = 0.0;

    int tipo = dtKeyLightType();

    if (tipo == 1)   // foco
    {
        // Bias en ESPACIO DE MUNDO, y no el bias de profundidad del
        // rasterizador.
        //
        // El del rasterizador esta afinado para la ortografica de las cascadas,
        // donde la profundidad NDC es lineal en la distancia real: un offset
        // fijo de buffer vale siempre los mismos metros. En perspectiva no, asi
        // que lejos de la bombilla se queda corto y la superficie se
        // auto-sombrea. Y corregirlo por ahi obligaria a un par de PSO extra en
        // CADA backend, porque el bias es estado de pipeline en los dos.
        //
        // Dos terminos, porque atacan cosas distintas:
        //
        //  - NORMAL-OFFSET, proporcional a la distancia a la luz. En
        //    perspectiva la huella de un texel del mapa CRECE con esa distancia,
        //    y el error de cuantizacion con ella; un offset fijo que vale cerca
        //    de la bombilla se queda corto lejos. Separarse a lo largo de la
        //    normal es lo que de verdad saca a la superficie de su propia
        //    sombra, porque el error esta en el plano de la superficie.
        //
        //  - Empuje HACIA LA LUZ, pequeno y constante. Es el que cubre el caso
        //    de una superficie vista casi de canto desde la luz, donde la normal
        //    apenas separa en profundidad.
        //
        // Se nota al MOVERSE mas que quieto porque el TAA acumula historia
        // mientras la camara esta parada y la rechaza en cuanto se mueve: sin
        // bias suficiente, el patron de acne cambia con el jitter subpixel y el
        // TAA ya no lo puede promediar.
        vec3  aLaLuz  = ubo.lights[0].position.xyz - worldPos;
        float distLuz = length(aLaLuz);
        if (distLuz > 1e-5)
        {
            worldPos += (aLaLuz / distLuz) * 0.02;
            worldPos += normalMundo * (distLuz * 0.004);
        }

        vec4 ls = ubo.lightSpaceMatrix[0] * vec4(worldPos, 1.0);
        // Detras de la luz: w <= 0 hace que la division devuelva el punto
        // reflejado, que caeria dentro del mapa y pintaria una sombra fantasma
        // al otro lado del foco.
        if (ls.w <= 0.0) return false;

        vec3 p = ls.xyz / ls.w;
        p.xy   = p.xy * 0.5 + 0.5;
        // Fuera del frustum del foco no hay nada grabado. El recorte en xy es
        // obligatorio aqui y no en las cascadas: el volumen de una cascada se
        // ajusta a lo que se ve, mientras que el cono de un foco deja fuera casi
        // toda la escena, y sin esta comprobacion el sampler estira el borde del
        // mapa por todo el resto del mundo.
        if (p.z < 0.0 || p.z > 1.0) return false;
        if (any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0)))) return false;

        uvz   = p;
        layer = 0.0;
        return true;
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
