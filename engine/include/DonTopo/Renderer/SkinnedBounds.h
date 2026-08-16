#pragma once

namespace DonTopo
{
    struct SkinnedMesh;

    // Cota de culling para las mallas con huesos, sin nada de ninguna API
    // grafica: vive al lado de Frustum.h por el mismo motivo que aquel, y por
    // el mismo camino (salio de Renderer cuando ese fichero se partio en pases).
    namespace Culling
    {
        // Radio de una esfera centrada en el ORIGEN LOCAL del modelo que
        // contiene la malla en CUALQUIER pose de cualquiera de sus clips.
        //
        // La AABB en reposo no vale: el compute deforma los vertices y un brazo
        // levantado se sale de la caja, asi que cullear con ella haria
        // desaparecer al personaje — el peor fallo posible aqui.
        //
        // No evalua ninguna pose: acota hueso a hueso con los valores extremos
        // de las keys y propaga por la jerarquia, asi que la cota vale tambien
        // entre keyframes. Es conservadora (puede sobrar), nunca corta de menos.
        //
        // Devuelve 0 si no hay con que acotar (sin huesos, sin vertices, o con
        // un NaN colado desde el modelo); el llamante lo trata como "sin cota" y
        // no culea.
        float skinnedBoundRadius(const SkinnedMesh& mesh);
    }  // namespace Culling
}  // namespace DonTopo
