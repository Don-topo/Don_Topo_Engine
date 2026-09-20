#include "DonTopo/Core/PropertyTracks.h"

#include <cmath>
#include <glm/glm.hpp>

namespace DonTopo
{
    namespace
    {
        // En el MISMO orden que PropertyId: el índice es el enum.
        const char* const kNombres[] = {
            "position.x", "position.y", "position.z",
            "rotation.x", "rotation.y", "rotation.z",
            "scale.x",    "scale.y",    "scale.z",
            "light.color.r", "light.color.g", "light.color.b",
            "light.intensity", "light.range",
            "material.metallic", "material.roughness",
        };
        static_assert(sizeof(kNombres) / sizeof(kNombres[0]) == (size_t)PropertyId::Count,
                      "la tabla de nombres tiene que cubrir PropertyId entero");
    }

    const char* propertyName(PropertyId id)
    {
        const int i = (int)id;
        return (i >= 0 && i < (int)PropertyId::Count) ? kNombres[i] : "";
    }

    PropertyId propertyFromName(const std::string& n)
    {
        for (int i = 0; i < (int)PropertyId::Count; i++)
            if (n == kNombres[i]) return (PropertyId)i;
        return PropertyId::Count;
    }

    bool propertyIsRotation(PropertyId id)
    {
        return id == PropertyId::RotationX || id == PropertyId::RotationY || id == PropertyId::RotationZ;
    }

    float samplePropertyTrack(const PropertyTrack& t, float tiempo, float actual)
    {
        if (t.keys.empty()) return actual;
        // El tramo se busca COMPARANDO tiempos, no por índice: el fichero (o el
        // panel) puede traer las keys en cualquier orden y el resultado no
        // puede depender de eso.
        const PropertyKey* lo = nullptr;
        const PropertyKey* hi = nullptr;
        for (const auto& k : t.keys)
        {
            if (k.time <= tiempo && (!lo || k.time > lo->time)) lo = &k;
            if (k.time >= tiempo && (!hi || k.time < hi->time)) hi = &k;
        }
        if (!lo) return hi->value;      // todas por delante: la primera
        if (!hi) return lo->value;      // todas por detrás: la última
        const float span = hi->time - lo->time;
        if (span <= 0.0f) return hi->value;
        return glm::mix(lo->value, hi->value, (tiempo - lo->time) / span);
    }

    float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n)
    {
        if (n <= 0) return 0.0f;
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += c[i].weight;
        if (total <= 0.0f) return 0.0f;
        if (!propertyIsRotation(id))
        {
            float v = 0.0f;
            for (int i = 0; i < n; i++) v += c[i].value * c[i].weight;
            return v / total;
        }
        // Ángulos: se mezcla la DIFERENCIA con la primera aportación,
        // normalizada a [-180, 180]. Mezclándolos en crudo, 350 y 10 darían
        // 180 (media aritmética) en vez de 0.
        const float base = c[0].value;
        float delta = 0.0f;
        for (int i = 0; i < n; i++)
            delta += std::remainder(c[i].value - base, 360.0f) * c[i].weight;
        return base + delta / total;
    }
}
