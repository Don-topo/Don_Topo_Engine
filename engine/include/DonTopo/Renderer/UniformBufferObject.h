#pragma once
#include <glm/glm.hpp>

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

    // Direccion en la que "cae" la luz key: la que construye el shadow map y la
    // que usa el in-scattering de la niebla. UN SOLO sitio a proposito — cuando
    // cada consumidor la derivaba por su cuenta, cambiar el criterio en las
    // cascadas y no en la niebla dejo el scattering apuntando a un lado y el
    // shadow map construido hacia otro.
    //
    //  - Direccional: su PROPIA direccion, que es el -Z local del GameObject.
    //  - Punto y foco: aproximacion de la luz hacia el origen del mundo. Es lo
    //    unico que dan de si unas sombras en cascada, que son de luz
    //    direccional; la sombra correcta de una luz de punto necesita un cubemap
    //    (ver P21 en docs/renderer-audit.md).
    //
    // false = no hay direccion utilizable (luz en el origen, o direccion nula) y
    // el llamante debe saltarse el pase en vez de dividir por cero.
    inline bool keyLightDirection(const glm::vec4& position, const glm::vec4& direction,
                                  glm::vec3& out)
    {
        // El tipo va en direction.w, con la misma convencion que usa pbr.frag:
        // int(w + 0.5).
        const int tipo = static_cast<int>(direction.w + 0.5f);

        if (tipo == static_cast<int>(LightType::Directional))
        {
            const glm::vec3 d(direction);
            const float     l = glm::length(d);
            if (l < 1e-6f) return false;
            out = d / l;
            return true;
        }

        const glm::vec3 p(position);
        const float     l = glm::length(p);
        if (l < 1e-6f) return false;
        out = -p / l;
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