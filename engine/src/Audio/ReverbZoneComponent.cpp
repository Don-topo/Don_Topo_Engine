#include "DonTopo/Audio/ReverbZoneComponent.h"

#include <algorithm>
#include <cmath>

namespace DonTopo
{
    // Mismo criterio que AudioClipComponent: se rechazan los no-finitos ANTES
    // del clamp (std::clamp(NaN, lo, hi) devuelve NaN) y el invariante
    // min <= max vive aquí, no en la UI — un .scene editado a mano tampoco
    // puede instalar una zona invertida.
    void ReverbZoneComponent::setMinDistance(float d)
    {
        if (!std::isfinite(d)) return;
        m_minDistance = std::clamp(d, 0.1f, 5000.0f);
        if (m_maxDistance < m_minDistance) m_maxDistance = m_minDistance;
    }

    void ReverbZoneComponent::setMaxDistance(float d)
    {
        if (!std::isfinite(d)) return;
        m_maxDistance = std::clamp(d, 1.0f, 10000.0f);
        if (m_maxDistance < m_minDistance) m_minDistance = m_maxDistance;
    }
}
