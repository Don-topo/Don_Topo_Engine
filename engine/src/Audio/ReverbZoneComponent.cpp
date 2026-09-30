#include "DonTopo/Audio/ReverbZoneComponent.h"

#include <algorithm>
#include <cmath>

namespace DonTopo
{
    // Same criterion as AudioClipComponent: non-finite values are rejected BEFORE
    // the clamp (std::clamp(NaN, lo, hi) returns NaN) and the invariant
    // min <= max lives here, not in the UI — a hand-edited .scene cannot
    // install an inverted zone either.
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
