#pragma once
#include <glm/glm.hpp>

#include <cmath>

namespace DonTopo
{
    // Frustum culling, sin nada de ninguna API gráfica: lo usan el backend de
    // Vulkan y el de DirectX 12, y lo usará el siguiente. Vivía dentro de
    // Renderer (Vulkan) hasta que hubo un segundo backend que lo necesitaba.
    namespace Culling
    {
        // Seis planos en espacio de mundo, con la normal apuntando HACIA DENTRO
        // del volumen: un punto es visible si queda del lado positivo de los
        // seis. Cada plano es (nx, ny, nz, d) con la normal normalizada, así que
        // dot(n, p) + d es la distancia con signo.
        struct Frustum {
            glm::vec4 planes[6] = {};
        };

        // Extrae los planos de una matriz viewProj (Gribb-Hartmann).
        inline Frustum frustumFromViewProj(const glm::mat4& m)
        {
            // glm es column-major: m[col][row]. Las filas de la matriz, que es
            // lo que necesita Gribb-Hartmann, hay que componerlas a mano.
            const glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
            const glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
            const glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
            const glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);

            Frustum f;
            f.planes[0] = r3 + r0;  // izquierda
            f.planes[1] = r3 - r0;  // derecha
            f.planes[2] = r3 + r1;  // abajo
            f.planes[3] = r3 - r1;  // arriba
            // Cercano por el convenio de OpenGL (r3 + r2) y NO por el de Vulkan
            // (r2 a secas), a propósito: este motor mezcla los dos rangos de
            // profundidad (el editor arma su proyección con glm::perspective,
            // que sin GLM_FORCE_DEPTH_ZERO_TO_ONE da z=[-1,1]; CameraComponent y
            // la luz usan *RH_ZO, que da z=[0,1]). Sobre una matriz ZO, r3+r2
            // describe un plano algo por DETRÁS del cercano real: recorta menos
            // de lo que podría, pero nunca descarta algo que se vería. Al revés
            // —r2 sobre una matriz [-1,1]— se comería la mitad cercana de la
            // escena, y el síntoma serían objetos que desaparecen al acercarse.
            f.planes[4] = r3 + r2;  // cercano
            f.planes[5] = r3 - r2;  // lejano

            // Normalizar: sin esto, dot(n,c)+d no es una distancia y el radio
            // proyectado de la AABB no sería comparable con ella.
            for (glm::vec4& p : f.planes)
            {
                const float len = glm::length(glm::vec3(p));
                if (len > 0.0f)
                    p /= len;
            }
            return f;
        }

        // AABB en espacio LOCAL del mesh + su transform a mundo. Devuelve false
        // solo si la caja queda entera fuera de algún plano; es un test
        // conservador (puede dar true de más en las esquinas del frustum, nunca
        // false de menos, que sería un objeto desaparecido).
        inline bool aabbVisible(const Frustum& frustum, const glm::vec3& localMin,
                                const glm::vec3& localMax, const glm::mat4& model)
        {
            // Centro + semiejes en vez de las 8 esquinas: el test por plano sale
            // en dos productos escalares en lugar de ocho.
            const glm::vec3 localCenter = (localMin + localMax) * 0.5f;
            const glm::vec3 localExtent = (localMax - localMin) * 0.5f;

            const glm::vec3 center = glm::vec3(model * glm::vec4(localCenter, 1.0f));

            // Semiejes de la AABB que envuelve a la caja ya transformada. El
            // valor absoluto de la 3x3 es lo que convierte una rotación en
            // "cuánto crece la caja alineada a ejes"; con escalado no uniforme
            // sale bien igual.
            const glm::mat3 rs = glm::mat3(model);
            const glm::vec3 extent(std::abs(rs[0][0]) * localExtent.x +
                                       std::abs(rs[1][0]) * localExtent.y +
                                       std::abs(rs[2][0]) * localExtent.z,
                                   std::abs(rs[0][1]) * localExtent.x +
                                       std::abs(rs[1][1]) * localExtent.y +
                                       std::abs(rs[2][1]) * localExtent.z,
                                   std::abs(rs[0][2]) * localExtent.x +
                                       std::abs(rs[1][2]) * localExtent.y +
                                       std::abs(rs[2][2]) * localExtent.z);

            for (const glm::vec4& p : frustum.planes)
            {
                const glm::vec3 n(p);
                const float     distance = glm::dot(n, center) + p.w;
                // Radio de la caja proyectado sobre la normal del plano: la
                // esquina que más sobresale hacia el lado positivo.
                const float radius = std::abs(n.x) * extent.x + std::abs(n.y) * extent.y +
                                     std::abs(n.z) * extent.z;
                if (distance + radius < 0.0f)
                    return false;  // entera del lado de fuera
            }
            return true;
        }
    }  // namespace Culling
}  // namespace DonTopo
