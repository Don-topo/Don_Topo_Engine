#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace DonTopo 
{
    constexpr int MAX_LIGHTS = 16;

    // Cascaded shadow maps: nº de cascadas del shadow map de la luz key. Tiene
    // que valer lo mismo aquí, en el array del bloque UBO de los shaders y en
    // las capas del texture array del shadow map: si se descuadra, std140
    // desplaza en silencio todo lo que va detrás de lightSpaceMatrix.
    constexpr int SHADOW_CASCADES = 4;

    // Matrices que hay que reservar en el SSBO de instancias de un frame, en el
    // peor caso: cada objeto visible en las CUATRO cascadas del shadow map, mas
    // el pase de escena, mas el pre-pase de profundidad que alimenta al SSAO y
    // a la niebla. De ahi el (SHADOW_CASCADES + 2).
    //
    // Los personajes cuentan igual que los estaticos y ese era el fallo (H23):
    // la cuenta salia solo de los estaticos, asi que una escena de puros
    // personajes reservaba CERO y el pase de sombras se salia por un `break`
    // mudo. Sin error, sin aviso, y la sombra sencillamente no estaba.
    //
    // Satura en vez de envolver: una cuenta absurda tiene que pedir DEMASIADO,
    // que falla al asignar y se ve, no poco, que vuelve al fallo silencioso.
    constexpr uint32_t shadowInstanceCapacity(uint32_t staticCount, uint32_t skinnedCount)
    {
        constexpr uint64_t kPasses = static_cast<uint64_t>(SHADOW_CASCADES) + 2;
        const uint64_t total = (static_cast<uint64_t>(staticCount) +
                                static_cast<uint64_t>(skinnedCount)) * kPasses;
        return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(total);
    }

    // Cuantas luces ADEMAS de la key pueden proyectar sombra, y cuanto cuesta
    // cada una.
    //
    // Solo FOCOS. Una direccional secundaria necesitaria sus propias 4 cascadas
    // para no verse peor que no tener sombra, y una de punto son 6 caras; el
    // foco es el unico tipo que cabe en UNA capa. Las demas luces siguen
    // iluminando, simplemente no arrojan sombra.
    //
    // Cuatro y no mas porque cada una es una capa del shadow map y un render
    // pass entero por frame: a 2048 son 16 MB y un recorrido mas de los
    // casters.
    constexpr int SHADOW_EXTRA_CASTERS = 4;

    // Huecos de matriz de sombra en el UBO, y capas del shadow map.
    //
    // Los seis primeros son de la luz KEY, que es la unica que puede usar mas de
    // uno: 4 si es direccional (una por cascada), 6 si es de punto o un foco muy
    // abierto (una por cara del cubemap), 1 si es un foco normal. Nunca coexisten
    // porque solo hay una luz key y solo tiene un tipo.
    //
    // Los SHADOW_EXTRA_CASTERS de detras son de un foco cada uno.
    //
    // Tiene que valer lo mismo aqui y en los SEIS shaders que declaran el
    // bloque UBO: si se descuadra, std140 desplaza en silencio todo lo que va
    // detras y no lo delata ninguna capa de validacion.
    constexpr int SHADOW_KEY_MATRICES = 6;
    constexpr int SHADOW_MATRICES     = SHADOW_KEY_MATRICES + SHADOW_EXTRA_CASTERS;

    // Tipo de luz. Va en direction.w (float) y no en un int aparte: std140
    // alinearia el int a 16 bytes igual, asi que ocupar el hueco que la vec4 ya
    // tenia libre sale gratis.
    enum class LightType : int { Point = 0, Spot = 1, Directional = 2, Area = 3 };

    struct Light
    {
        glm::vec4 position {0.0f, 0.0f, 0.0f, 0.0f};    // xyz mundo, w unused
        glm::vec4 color { 1.0f, 1.0f, 1.0f, 1.0f};      // rgb = color, a = intensity
        // xyz = direccion normalizada (-Z local del GameObject), w = tipo
        // (0 point, 1 spot, 2 directional, 3 area).
        glm::vec4 direction {0.0f, -1.0f, 0.0f, 0.0f};
        // x = range, y = cos(angulo interior), z = cos(angulo exterior),
        // w = ancho del area.
        glm::vec4 params {10.0f, 0.9f, 0.7f, 1.0f};
    };

    // Punto al que apunta una luz sin direccion propia: el centro de la ESCENA,
    // como media de los origenes de lo que hay dibujado.
    //
    // Estable a proposito. La version anterior apuntaba al centro del frustum de
    // la camara, que suena mejor —la sombra cae donde estas mirando— pero ata la
    // direccion de la luz a la camara: girar en el sitio giraba la sombra, y eso
    // se ve MUCHO peor que el problema que arreglaba. Con el centro de la escena
    // la sombra solo se mueve cuando se mueve la luz, que es lo que uno espera.
    //
    // Los origenes y no las cajas envolventes: esto solo elige la direccion de
    // una aproximacion, y un centroide exacto no la mejoraria en nada
    // apreciable. Es el mismo criterio que ya usaba el rango de camara de D3D12
    // con los personajes.
    struct SceneCenter
    {
        glm::vec3 suma{0.0f};
        int       n = 0;

        void add(const glm::vec3& origen) { suma += origen; ++n; }
        // La cuarta columna de un transform de mundo, que es el caso de los dos
        // backends.
        void add(const glm::mat4& transform) { add(glm::vec3(transform[3])); }

        // false = escena vacia. El llamante decide: las cascadas se saltan el
        // pase (no hay nada que sombrear) y la niebla se queda con su direccion
        // neutra.
        bool get(glm::vec3& out) const
        {
            if (n == 0) return false;
            out = suma / static_cast<float>(n);
            return true;
        }
    };

    // Plano cercano del shadow map en perspectiva de un foco. Constante y no
    // ajustable: lo unico que se nota al subirlo es que un caster pegado a la
    // bombilla deja de proyectar, y bajarlo mas reparte la precision de z aun
    // peor. Vive aqui porque los dos backends tienen que usar EXACTAMENTE el
    // mismo, o el bias de sombra cuadra en uno y no en el otro.
    constexpr float SPOT_SHADOW_NEAR = 0.05f;

    // Matriz de sombra de un FOCO: proyeccion en PERSPECTIVA desde su posicion,
    // con el FOV de su cono. Es lo que hace que su sombra DIVERJA —crezca al
    // alejarse de la luz— en vez de mantener el tamano como la aproximacion en
    // cascada, que es de luz direccional.
    //
    // Va en lightSpaceMatrix[0] y se graba en la capa 0 del mismo shadow map de
    // siempre: las cascadas y esto no coexisten nunca, porque solo hay una luz
    // key y solo tiene un tipo. Por eso no hace falta ni un recurso nuevo ni un
    // binding nuevo.
    //
    // flipY: quien absorbe la convencion de Y del backend. Los dos dejan el mapa
    // en la MISMA orientacion, que es la que el muestreo compartido da por
    // supuesta, pero por caminos distintos:
    //
    //   Vulkan  -> flipY = true.  Lo hace la matriz, igual que la ortografica de
    //                             las cascadas (ShadowPass.cpp).
    //   D3D12   -> flipY = false. Lo hace el viewport de altura negativa del
    //                             pase de sombras (shadowViewport.Height < 0).
    //
    // Poner los DOS o NINGUNO no da error en ninguna capa de validacion y el
    // sintoma no es una sombra desplazada, que se veria enseguida: es un
    // titileo. Con el mapa espejado en v la comparacion de profundidad cae en un
    // texel que no tiene nada que ver, asi que sombra y luz salen casi al azar
    // sobre la superficie y el jitter del TAA los remueve en cada frame — se
    // confunde facil con falta de bias, y subir el bias no lo toca.
    //
    // false = la luz no tiene direccion utilizable y el llamante debe saltarse
    // el pase.
    // FOV que necesitaria el shadow map de un foco, en radianes. Por encima de
    // 90 grados una sola cara deja de ser la tecnica adecuada: la huella de un
    // texel es 2*d*tan(FOV/2)/resolucion, y tan() se dispara cerca de 180 —a 150
    // grados es 3,7 veces peor que a 90, y a 175 son 23 veces—. De ahi el umbral
    // de spotNecesitaCubemap.
    inline float spotShadowFov(const glm::vec4& params)
    {
        // params.z = COSENO del angulo exterior del cono, o sea el semiangulo.
        // El FOV del mapa es el angulo completo (el doble) y ademas con margen:
        // sin el, el borde del cono cae justo en el borde del mapa y los taps
        // del PCF se salen por un lado.
        const float cosOuter = glm::clamp(params.z, -0.9999f, 0.9999f);
        return glm::clamp(2.0f * std::acos(cosOuter) * 1.15f,
                          glm::radians(5.0f), glm::radians(175.0f));
    }

    // Un foco tan abierto que ya no cabe bien en una cara: se le da el cubemap de
    // seis, igual que a una luz de punto. Su cono sigue recortando la LUZ en el
    // shader; lo unico que cambia es que el shadow map cubre mas de lo que hace
    // falta, a cambio de que ninguna cara pase de 90 grados.
    inline bool spotNecesitaCubemap(const glm::vec4& params)
    {
        return spotShadowFov(params) > glm::radians(90.0f);
    }

    inline bool spotShadowMatrix(const glm::vec4& position, const glm::vec4& direction,
                                 const glm::vec4& params, bool flipY, glm::mat4& out)
    {
        const glm::vec3 d(direction);
        const float     l = glm::length(d);
        if (l < 1e-6f) return false;
        const glm::vec3 dir = d / l;
        const glm::vec3 pos(position);

        const float fov = spotShadowFov(params);

        // params.x = alcance de la luz. Mas alla no ilumina, asi que tampoco hay
        // sombra suya que grabar, y acotar el far ahi es lo que le da precision
        // de profundidad al trozo que si se usa.
        const float lejos = (std::max)(params.x, SPOT_SHADOW_NEAR * 2.0f);

        const glm::vec3 up = std::abs(dir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                     : glm::vec3(0.0f, 1.0f, 0.0f);

        // *RH_ZO y no glm::perspective a secas: a secas da z en [-1,1] y Vulkan
        // clipea la mitad cercana.
        glm::mat4 proj = glm::perspectiveRH_ZO(fov, 1.0f, SPOT_SHADOW_NEAR, lejos);
        // Sobre la PROYECCION y antes de multiplicar. Hacerlo despues, sobre
        // out[1][1], seria otra cosa: en el producto la fila 1 ya lleva mezclada
        // la vista, y negar un solo elemento de esa fila no equivale a negarla
        // entera.
        if (flipY) proj[1][1] *= -1.0f;

        out = proj * glm::lookAt(pos, pos + dir, up);
        return true;
    }

    // Las SEIS matrices del cubemap de sombras de una luz de PUNTO: una por
    // cara, proyeccion en perspectiva de 90 grados desde su posicion. Es lo que
    // hace que su sombra diverja en TODAS las direcciones, que es lo que hace una
    // bombilla; las cascadas solo sabian proyectar en paralelo a lo largo de una.
    //
    // Orden de cara: 0 = +X, 1 = -X, 2 = +Y, 3 = -Y, 4 = +Z, 5 = -Z. El shader
    // elige cara por el eje MAYOR de (fragmento - luz) y luego proyecta con
    // ESTA MISMA matriz, no con una formula suya. Es deliberado: derivar la UV a
    // mano obliga a que dos convenciones de cubemap coincidan, y en este motor
    // eso ya salio mal una vez. Asi lo unico que hay que acertar es CUAL de las
    // seis, y equivocarse se ve como un corte duro, no como basura sutil.
    //
    // Por eso mismo el vector "up" de cada cara da igual mientras sea el mismo
    // aqui y al grabar: solo gira la cara sobre si misma, y la matriz que
    // deshace ese giro es la que se usa para muestrear.
    //
    // 90 grados exactos y aspecto 1: es lo unico que hace que las seis caras
    // cubran la esfera entera sin hueco ni solape.
    //
    // flipY: lo mismo que en spotShadowMatrix — Vulkan true, D3D12 false.
    // false = la luz no tiene alcance utilizable.
    inline bool pointShadowMatrices(const glm::vec4& position, const glm::vec4& params,
                                    bool flipY, glm::mat4* out /*[6]*/)
    {
        const glm::vec3 pos(position);
        const float     lejos = (std::max)(params.x, SPOT_SHADOW_NEAR * 2.0f);
        if (!std::isfinite(lejos) || lejos <= SPOT_SHADOW_NEAR) return false;

        glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f,
                                               SPOT_SHADOW_NEAR, lejos);
        if (flipY) proj[1][1] *= -1.0f;

        static const glm::vec3 kDir[6] = {
            { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
            { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
            { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        };
        // Para +Y y -Y el up no puede ser (0,1,0) o lookAt degenera.
        static const glm::vec3 kUp[6] = {
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
        };

        for (int f = 0; f < 6; f++)
            out[f] = proj * glm::lookAt(pos, pos + kDir[f], kUp[f]);
        return true;
    }

    // Margen por detras del volumen de cada cascada, en la direccion de la luz.
    // Sin el, un objeto alto que queda fuera del frustum de la camara pero cuya
    // sombra si cae dentro no se dibujaria en el shadow map.
    constexpr float SHADOW_CASTER_MARGIN = 200.0f;

    // Reparto de cascadas de una luz DIRECCIONAL: los cortes de profundidad y la
    // matriz ortografica de cada una.
    //
    // Estaba escrito DOS VECES, una por backend, con la misma matematica y las
    // mismas constantes. Nadie detectaba una divergencia: un ajuste en una copia
    // dejaba las sombras distintas en el otro backend y solo se veia poniendo
    // las dos escenas lado a lado (H3).
    //
    //   view/proj      camara del frame. proj puede llevar el Y-flip de Vulkan
    //                  dentro o no: aqui solo sirve para desproyectar el
    //                  frustum, y el cubo NDC es simetrico.
    //   lightDir       direccion ya resuelta por keyLightDirection.
    //   maxDistance    alcance de las sombras (RendererState::shadowDistance).
    //   lambda         mezcla entre reparto logaritmico y uniforme.
    //   shadowMapSize  lado del mapa, para el snap a texeles.
    //   flipY          igual que en spotShadowMatrix: Vulkan true, D3D12 false.
    //
    // false = la proyeccion es degenerada o el alcance no llega ni al near, y el
    // llamante debe dejar las matrices como estaban.
    inline bool cascadeShadowMatrices(const glm::mat4& view, const glm::mat4& proj,
                                      const glm::vec3& lightDir,
                                      float maxDistance, float lambda,
                                      uint32_t shadowMapSize, bool flipY,
                                      glm::mat4* outMatrices /*[SHADOW_CASCADES]*/,
                                      glm::vec4& outSplits)
    {
        if (shadowMapSize == 0) return false;

        // Esquinas del frustum, desproyectando el cubo NDC. z va de 0 a 1 y no
        // de -1 a 1 porque ese es el rango que clipean Vulkan y D3D12: lo que se
        // dibuja de verdad esta siempre entre esos dos planos.
        const glm::mat4 invViewProj = glm::inverse(proj * view);
        glm::vec3       cornerNear[4], cornerFar[4];
        const float     ndcX[4] = { -1.0f,  1.0f,  1.0f, -1.0f };
        const float     ndcY[4] = { -1.0f, -1.0f,  1.0f,  1.0f };
        for (int i = 0; i < 4; i++)
        {
            const glm::vec4 pn = invViewProj * glm::vec4(ndcX[i], ndcY[i], 0.0f, 1.0f);
            const glm::vec4 pf = invViewProj * glm::vec4(ndcX[i], ndcY[i], 1.0f, 1.0f);
            if (std::abs(pn.w) < 1e-8f || std::abs(pf.w) < 1e-8f) return false;
            cornerNear[i] = glm::vec3(pn) / pn.w;
            cornerFar[i]  = glm::vec3(pf) / pf.w;
        }

        // near/far REALES: la profundidad en view space de esos dos planos. No
        // se sacan de los coeficientes de proj a proposito — el editor construye
        // su proyeccion con glm::perspective (z en [-1,1]) y el CameraComponent
        // con *RH_ZO, asi que los mismos coeficientes significan cosas distintas
        // y la formula tendria que saber cual esta activa. Los planos z=0 y z=1,
        // en cambio, son los mismos en los dos casos.
        const float camNear = -(view * glm::vec4(cornerNear[0], 1.0f)).z;
        const float camFar  = -(view * glm::vec4(cornerFar[0],  1.0f)).z;
        if (!std::isfinite(camNear) || !std::isfinite(camFar) ||
            camNear <= 0.0f || camFar <= camNear)
        {
            return false;
        }

        // Las esquinas ya estan puestas con el far REAL (es el que define los
        // rayos del frustum); el reparto de cascadas usa el far recortado.
        const float shadowFar = (std::min)(camFar, maxDistance);
        if (shadowFar <= camNear) return false;

        const glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f)
                                                          : glm::vec3(0.0f, 1.0f, 0.0f);
        const glm::mat4 lightRot    = glm::lookAt(glm::vec3(0.0f), lightDir, up);
        const glm::mat4 invLightRot = glm::inverse(lightRot);

        float prevDist = camNear;
        for (int c = 0; c < SHADOW_CASCADES; c++)
        {
            const float p        = (float)(c + 1) / (float)SHADOW_CASCADES;
            const float logSplit = camNear * std::pow(shadowFar / camNear, p);
            const float uniSplit = camNear + (shadowFar - camNear) * p;
            const float dist     = lambda * logSplit + (1.0f - lambda) * uniSplit;
            outSplits[c]         = dist;

            // Interpolar entre las esquinas cercana y lejana es exacto: la
            // profundidad en view space varia linealmente a lo largo de ese
            // segmento. Los factores se calculan contra el far REAL porque es el
            // que situa cornerFar.
            const float tNear = (prevDist - camNear) / (camFar - camNear);
            const float tFar  = (dist     - camNear) / (camFar - camNear);

            glm::vec3 corners[8];
            for (int i = 0; i < 4; i++)
            {
                const glm::vec3 ray = cornerFar[i] - cornerNear[i];
                corners[i]     = cornerNear[i] + ray * tNear;
                corners[i + 4] = cornerNear[i] + ray * tFar;
            }

            // Esfera envolvente y no AABB: el radio no depende de hacia donde
            // mire la camara, asi que girar en el sitio no cambia el tamano del
            // volumen y las sombras no laten.
            glm::vec3 center(0.0f);
            for (const glm::vec3& v : corners) center += v;
            center /= 8.0f;
            float radius = 0.0f;
            for (const glm::vec3& v : corners)
                radius = (std::max)(radius, glm::length(v - center));
            // Cuantizar el radio evita que un cambio minimo de la camara mueva
            // el borde del volumen y con el todos los texeles.
            radius = std::ceil(radius * 16.0f) / 16.0f;
            if (radius < 1e-4f) radius = 1e-4f;

            // Snap del centro a texeles del shadow map, en el espacio de la luz.
            // Sin esto, avanzar la camara arrastra el volumen de forma continua
            // y los bordes de sombra hierven.
            const float unitsPerTexel = (2.0f * radius) / (float)shadowMapSize;
            glm::vec3   centerLS      = glm::vec3(lightRot * glm::vec4(center, 1.0f));
            centerLS.x = std::floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
            centerLS.y = std::floor(centerLS.y / unitsPerTexel) * unitsPerTexel;
            center     = glm::vec3(invLightRot * glm::vec4(centerLS, 1.0f));

            const glm::mat4 lightView =
                glm::lookAt(center - lightDir * (radius + SHADOW_CASTER_MARGIN), center, up);
            glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius,
                                                  0.0f, 2.0f * radius + SHADOW_CASTER_MARGIN);
            // Sobre la PROYECCION y antes de multiplicar, igual que en
            // spotShadowMatrix: en el producto la fila 1 ya lleva mezclada la
            // vista y negar un solo elemento no equivale a negarla entera.
            if (flipY) lightProj[1][1] *= -1.0f;

            outMatrices[c] = lightProj * lightView;
            prevDist       = dist;
        }
        return true;
    }

    // Reparte las ranuras de sombra entre las luces que NO son la key.
    //
    // Recorre las luces 1..n-1 en orden de escena y le da una ranura a cada FOCO
    // hasta agotar SHADOW_EXTRA_CASTERS. En orden de escena y no por brillo o
    // cercania a proposito: si el criterio dependiera de la camara, una luz
    // ganaria y perderia su sombra al moverte y eso parpadea.
    //
    // ranuraDeLuz[i] = indice de matriz/capa de la luz i, o -1 si no proyecta.
    // La luz 0 siempre sale -1: sus matrices las pone computeCascades y ocupan
    // los SHADOW_KEY_MATRICES primeros huecos.
    //
    // Devuelve cuantas ranuras se ocuparon.
    //
    // Vive aqui y no en cada backend porque el reparto tiene que ser IDENTICO en
    // los dos: la ranura decide en que capa se graba y con que matriz muestrea el
    // shader, asi que un reparto distinto por backend seria la misma escena con
    // sombras en luces distintas.
    template <typename LuzT, typename TipoDeLuz, typename ParamsDeLuz>
    inline int repartirSombrasExtra(const LuzT* luces, int n,
                                    TipoDeLuz tipoDe, ParamsDeLuz paramsDe,
                                    int* ranuraDeLuz /*[n]*/)
    {
        for (int i = 0; i < n; i++) ranuraDeLuz[i] = -1;

        int usadas = 0;
        for (int i = 1; i < n && usadas < SHADOW_EXTRA_CASTERS; i++)
        {
            if (tipoDe(luces[i]) != static_cast<int>(LightType::Spot)) continue;
            // Un foco tan abierto que necesitaria cubemap no cabe en una capa.
            // Sigue iluminando; solo no proyecta.
            if (spotNecesitaCubemap(paramsDe(luces[i]))) continue;

            ranuraDeLuz[i] = SHADOW_KEY_MATRICES + usadas;
            ++usadas;
        }
        return usadas;
    }

    // Direccion en la que "cae" la luz key: la que construye el shadow map y la
    // que usa el in-scattering de la niebla. UN SOLO sitio a proposito — cuando
    // cada consumidor la derivaba por su cuenta, cambiar el criterio en las
    // cascadas y no en la niebla dejo el scattering apuntando a un lado y el
    // shadow map construido hacia otro.
    //
    //  - Direccional y foco: su PROPIA direccion, que es el -Z local del
    //    GameObject. Un foco tiene cono, o sea que tiene direccion de verdad;
    //    antes se le aplicaba la aproximacion de la luz de punto y girar su
    //    gizmo no movia su sombra.
    //  - Punto: no tiene direccion, asi que se apunta de la luz al centro de la
    //    escena (aim). Es lo unico que dan de si unas sombras en cascada, que
    //    son de luz direccional: la proyeccion sigue siendo paralela y el tamano
    //    de la sombra no cambia con la distancia. La sombra correcta necesita un
    //    cubemap (ver P21 en docs/renderer-audit.md).
    //
    // aim sale de SceneCenter. Pasarle el origen del mundo reproduce el
    // comportamiento anterior, que solo cuadraba con la escena centrada ahi.
    //
    // false = no hay direccion utilizable (luz justo en el punto de mira, o
    // direccion nula) y el llamante debe saltarse el pase en vez de dividir por
    // cero.
    inline bool keyLightDirection(const glm::vec4& position, const glm::vec4& direction,
                                  const glm::vec3& aim, glm::vec3& out)
    {
        // El tipo va en direction.w, con la misma convencion que usa pbr.frag:
        // int(w + 0.5).
        const int tipo = static_cast<int>(direction.w + 0.5f);

        if (tipo == static_cast<int>(LightType::Directional) ||
            tipo == static_cast<int>(LightType::Spot))
        {
            const glm::vec3 d(direction);
            const float     l = glm::length(d);
            if (l < 1e-6f) return false;
            out = d / l;
            return true;
        }

        const glm::vec3 haciaElCentro = aim - glm::vec3(position);
        const float     l             = glm::length(haciaElCentro);
        if (l < 1e-6f) return false;
        out = haciaElCentro / l;
        return true;
    }

    struct UniformBufferObject
    {
        glm::mat4   view;
        glm::mat4   proj;
        glm::mat4   lightSpaceMatrix[SHADOW_MATRICES];
        // Distancia (view space, positiva) hasta la que llega cada cascada. La
        // última es el alcance total de las sombras: más allá, el fragment
        // shader devuelve "sin sombra" en vez de muestrear.
        glm::vec4   cascadeSplits{0.0f};
        Light       lights[MAX_LIGHTS];
        glm::vec4   viewPos;
        int         numLights = 0;
        // Multiplicador global del ambiente (IBL). Ocupa el PRIMER hueco del
        // padding que ya existía tras numLights, así que ni sizeof(UBO) ni el
        // offset de ningún miembro anterior cambian: solo pbr.frag declara este
        // campo, y los otros 4 shaders que comparten el bloque siguen viendo
        // exactamente el mismo layout std140 que antes.
        float       ambientIntensity = 1.0f;
        float       _pad[2]{};              // std140: alinear a 16 bytes tras el int
    };

    /*
        (Light ocupa 4×vec4=64 bytes, ya alineado a 16.
        El int numLights tras el array necesita padding de 12 
        bytes para que el siguiente miembro—si lo hubiera—respete std140; 
        aquí es el último campo así que el padding solo asegura sizeof(UBO) 
        múltiplo de 16, lo cual ya cumple mat4+mat4+4*32+16+4 = ...)
    */
}