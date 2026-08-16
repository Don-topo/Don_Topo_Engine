#include "DonTopo/Renderer/SkinnedBounds.h"
#include "DonTopo/Renderer/SkinnedMesh.h"

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

namespace DonTopo::Culling
{
    // Mayor factor por el que la 3x3 de m puede estirar un vector, o sea su
    // mayor valor singular. Sirve para acotar cuánto separa una matriz de hueso
    // a un vértice del origen de ese hueso.
    //
    // Se calcula exacto (autovalores de m^T·m por la forma cerrada de una
    // simétrica 3x3) en vez de con una cota fácil: la norma de Frobenius vale
    // sqrt(3) para la IDENTIDAD, y como esto se multiplica a lo largo de la
    // cadena de huesos, un esqueleto de diez niveles saldría 240 veces más
    // grande de lo que es y no se culearía nunca.
    static float operatorNorm3(const glm::mat4& m)
    {
        const glm::mat3 r  = glm::mat3(m);
        const glm::mat3 a  = glm::transpose(r) * r;   // simétrica y semidefinida positiva
        // Cualquier elemento de la diagonal es una cota INFERIOR del mayor
        // autovalor (cociente de Rayleigh sobre los ejes). Se usa de red: si la
        // forma cerrada se va por redondeo, el resultado sigue sin quedarse corto.
        float lambda = std::max(a[0][0], std::max(a[1][1], a[2][2]));

        const float p1 = a[1][0]*a[1][0] + a[2][0]*a[2][0] + a[2][1]*a[2][1];
        if (p1 > 0.0f)
        {
            const float q  = (a[0][0] + a[1][1] + a[2][2]) / 3.0f;
            const float p2 = (a[0][0]-q)*(a[0][0]-q) + (a[1][1]-q)*(a[1][1]-q)
                           + (a[2][2]-q)*(a[2][2]-q) + 2.0f*p1;
            const float p  = std::sqrt(p2 / 6.0f);
            if (p > 0.0f)
            {
                const glm::mat3 b   = (a - q * glm::mat3(1.0f)) * (1.0f / p);
                const float     det = glm::determinant(b);
                const float     phi = std::acos(std::clamp(det * 0.5f, -1.0f, 1.0f)) / 3.0f;
                lambda = std::max(lambda, q + 2.0f * p * std::cos(phi));
            }
        }
        return (lambda > 0.0f) ? std::sqrt(lambda) : 0.0f;
    }

    float skinnedBoundRadius(const SkinnedMesh& mesh)
    {
        const Skeleton& skel      = mesh.skeleton;
        const int       boneCount = (int)skel.names.size();
        if (boneCount <= 0 || mesh.skinnedVertices.empty()) return 0.0f;
        if ((int)skel.parentIndex.size() < boneCount ||
            (int)skel.inverseBindPose.size() < boneCount) return 0.0f;

        // r[b]: radio de la nube de vértices que arrastra el hueso b, medido en
        // el espacio del PROPIO hueso — por eso el inverseBindPose. Es
        // invariante a la pose: skinning.comp le aplica encima la matriz de
        // mundo del hueso y nada más, así que la distancia al origen del hueso
        // sólo puede crecer por la escala, que se contabiliza aparte.
        std::vector<float> radio((size_t)boneCount, 0.0f);
        // Los pesos de un vértice deberían sumar 1, pero un FBX puede traerlos
        // sin normalizar y skinning.comp no los normaliza: sumar más de 1
        // alejaría el vértice más allá de la envolvente convexa de los huesos.
        float maxPeso = 1.0f;
        for (const SkinnedVertex& v : mesh.skinnedVertices)
        {
            float suma = 0.0f;
            for (int i = 0; i < 4; i++)
            {
                const float w = v.boneWeights[i];
                if (w <= 0.0f) continue;              // misma condición que skinning.comp
                const int b = v.boneIndices[i];
                if (b < 0 || b >= boneCount) continue; // índice basura: el shader ya leería fuera
                suma += w;
                const glm::vec3 p = glm::vec3(skel.inverseBindPose[b] *
                                              glm::vec4(glm::vec3(v.position), 1.0f));
                radio[(size_t)b] = std::max(radio[(size_t)b], glm::length(p));
            }
            maxPeso = std::max(maxPeso, suma);
        }

        // Cota del transform LOCAL de cada hueso, tomando el peor clip. Sólo se
        // miran los extremos de las keys: eso es lo que hace que la cota valga
        // también entre keyframes, sin muestrear poses.
        std::vector<float> maxTrans((size_t)boneCount, 0.0f);
        std::vector<float> maxEscala((size_t)boneCount, 0.0f);
        const size_t clipCount = mesh.animationClips.empty() ? 1u : mesh.animationClips.size();
        for (int b = 0; b < boneCount; b++)
        {
            // Local de bind pose, el default de un hueso del que el clip no dice
            // nada (mismo cálculo y mismo motivo que packSkinnedClips).
            const glm::mat4 globalBind = glm::inverse(skel.inverseBindPose[b]);
            const int       padre      = skel.parentIndex[b];
            const glm::mat4 bindLocal  = (padre < 0) ? globalBind
                                                     : skel.inverseBindPose[padre] * globalBind;
            // El mayor de los vectores columna NO vale como cota: si el FBX trae
            // shear las columnas no son ortogonales y se quedaría corto.
            const float bindTrans  = glm::length(glm::vec3(bindLocal[3]));
            const float bindEscala = operatorNorm3(bindLocal);

            for (size_t c = 0; c < clipCount; c++)
            {
                const BoneChannel* ch = nullptr;
                if (!mesh.animationClips.empty())
                    for (const BoneChannel& cc : mesh.animationClips[c].channels)
                        if (cc.boneIndex == b) { ch = &cc; break; }

                // Sin canal, o con canal pero sin ninguna key: bone_eval.comp
                // cae al bindLocal entero.
                const bool sinKeys = !ch || (ch->posKeys.empty() && ch->rotKeys.empty() &&
                                             ch->scaleKeys.empty());
                if (sinKeys)
                {
                    maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  bindTrans);
                    maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], bindEscala);
                    continue;
                }
                // Con keys de otro canal pero sin las suyas, bone_eval usa los
                // neutros del shader: posición 0 y escala 1, NO los del bind.
                float t = 0.0f;
                for (const BoneKeyframe& k : ch->posKeys) t = std::max(t, glm::length(k.value));
                float s = 1.0f;
                for (const BoneKeyframe& k : ch->scaleKeys)
                    s = std::max(s, std::max(std::abs(k.value.x),
                                  std::max(std::abs(k.value.y), std::abs(k.value.z))));
                maxTrans[(size_t)b]  = std::max(maxTrans[(size_t)b],  t);
                maxEscala[(size_t)b] = std::max(maxEscala[(size_t)b], s);
            }
        }

        // Propagación por la jerarquía. alcance[b] = distancia máxima del origen
        // del hueso al origen del modelo; cadena[b] = cota de la escala
        // acumulada desde la raíz. La traslación local de b la aplica la parte
        // lineal del PADRE, de ahí que use cadena[padre] y no cadena[b].
        //
        // Se apoya en que el padre va antes que el hijo, el mismo orden
        // topológico del que ya depende bone_hierarchy.comp.
        std::vector<float> alcance((size_t)boneCount, 0.0f);
        std::vector<float> cadena((size_t)boneCount, 1.0f);
        float R = 0.0f;
        for (int b = 0; b < boneCount; b++)
        {
            const int   padre       = skel.parentIndex[b];
            const bool  tienePadre  = (padre >= 0 && padre < b);
            const float alcancePadre = tienePadre ? alcance[(size_t)padre] : 0.0f;
            const float cadenaPadre  = tienePadre ? cadena[(size_t)padre]  : 1.0f;
            alcance[(size_t)b] = alcancePadre + cadenaPadre * maxTrans[(size_t)b];
            cadena[(size_t)b]  = cadenaPadre * maxEscala[(size_t)b];
            R = std::max(R, alcance[(size_t)b] + cadena[(size_t)b] * radio[(size_t)b]);
        }

        R *= maxPeso;
        // Un NaN o un infinito colados desde el modelo harían pasar el test de
        // culling de forma impredecible: mejor devolver "sin cota".
        return std::isfinite(R) ? R : 0.0f;
    }
}  // namespace DonTopo::Culling
