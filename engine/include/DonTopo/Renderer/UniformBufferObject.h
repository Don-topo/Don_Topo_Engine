#pragma once
#include <glm/glm.hpp>
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
        glm::mat4   lightSpaceMatrix[SHADOW_CASCADES];
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