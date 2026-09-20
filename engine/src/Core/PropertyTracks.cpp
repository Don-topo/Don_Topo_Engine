#include "DonTopo/Core/PropertyTracks.h"

#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// matrix_decompose y quaternion son extensiones GTX: sin la macro, glm avisa
// en cada unidad que las incluya.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

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

    float blendScalarValues(const PropertyContribution* c, int n)
    {
        if (n <= 0) return 0.0f;
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += c[i].weight;
        if (total <= 0.0f) return 0.0f;
        float v = 0.0f;
        for (int i = 0; i < n; i++) v += c[i].value * c[i].weight;
        return v / total;
    }

    float blendPropertyValues(PropertyId id, const PropertyContribution* c, int n)
    {
        if (n <= 0) return 0.0f;
        if (!propertyIsRotation(id)) return blendScalarValues(c, n);
        float total = 0.0f;
        for (int i = 0; i < n; i++) total += c[i].weight;
        if (total <= 0.0f) return 0.0f;
        // Ángulos: se mezcla la DIFERENCIA con la primera aportación,
        // normalizada a [-180, 180]. Mezclándolos en crudo, 350 y 10 darían
        // 180 (media aritmética) en vez de 0.
        const float base = c[0].value;
        float delta = 0.0f;
        for (int i = 0; i < n; i++)
            delta += std::remainder(c[i].value - base, 360.0f) * c[i].weight;
        return base + delta / total;
    }

    namespace
    {
        bool esDeTransform(PropertyId id) { return (int)id <= (int)PropertyId::ScaleZ; }
        bool esDeLuz(PropertyId id)
        {
            return (int)id >= (int)PropertyId::LightColorR && (int)id <= (int)PropertyId::LightRange;
        }

        // Traslación, euler en GRADOS y escala del localTransform.
        struct Trs { glm::vec3 pos{0.0f}, euler{0.0f}, escala{1.0f}; };
        Trs descompone(const glm::mat4& m)
        {
            Trs out;
            glm::quat rot;
            glm::vec3 skew;
            glm::vec4 persp;
            // glm::decompose con una matriz singular NO escribe sus salidas, así
            // que las locales van inicializadas arriba: leerlas sin más daría
            // basura (-1e8) y de ahí un NaN que se propaga a la escena.
            glm::decompose(m, out.escala, rot, out.pos, skew, persp);
            out.euler = glm::degrees(glm::eulerAngles(rot));
            return out;
        }
        glm::mat4 recompone(const Trs& t)
        {
            return glm::translate(glm::mat4(1.0f), t.pos) *
                   glm::mat4_cast(glm::quat(glm::radians(t.euler))) *
                   glm::scale(glm::mat4(1.0f), t.escala);
        }
        float componente(const glm::vec3& v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

        // Factores de material del OBJETO, no de la malla: editMesh() copia la
        // malla si está compartida, y animar no puede pagar eso cada frame.
        const MaterialOverride* overrideDe(const GameObject& go)
        {
            for (const auto& ov : go.materialOverrides)
                if (ov.index == 0) return &ov;
            return nullptr;
        }
        MaterialOverride& overrideParaEscribir(GameObject& go)
        {
            for (auto& ov : go.materialOverrides)
                if (ov.index == 0) return ov;
            MaterialOverride nuevo;
            nuevo.index = 0;
            go.materialOverrides.push_back(nuevo);
            return go.materialOverrides.back();
        }
    }

    bool propertyAvailable(const GameObject& go, PropertyId id)
    {
        if (esDeLuz(id)) return go.getLight() != nullptr;
        // El transform lo tiene todo GameObject, y los factores de material
        // viven en el propio objeto: no hacen falta ni malla ni componente.
        return id != PropertyId::Count;
    }

    float propertyGet(const GameObject& go, PropertyId id)
    {
        if (esDeTransform(id))
        {
            const Trs t = descompone(go.localTransform);
            const int i = (int)id % 3;
            if ((int)id <= (int)PropertyId::PositionZ) return componente(t.pos, i);
            if ((int)id <= (int)PropertyId::RotationZ) return componente(t.euler, i);
            return componente(t.escala, i);
        }
        if (esDeLuz(id))
        {
            const auto& luz = go.getLight();
            if (!luz) return 0.0f;
            switch (id)
            {
                case PropertyId::LightColorR:    return luz->getColor().x;
                case PropertyId::LightColorG:    return luz->getColor().y;
                case PropertyId::LightColorB:    return luz->getColor().z;
                case PropertyId::LightIntensity: return luz->getIntensity();
                default:                         return luz->getRange();
            }
        }
        // Material: el override si está activo (-1 es el centinela de "sin
        // override"), y si no el valor de la malla.
        const MaterialOverride* ov = overrideDe(go);
        const float delOverride = !ov ? -1.0f
                                      : (id == PropertyId::MaterialMetallic ? ov->metallic : ov->roughness);
        if (delOverride >= 0.0f) return delOverride;
        const auto& mesh = go.getMesh();
        if (!mesh) return id == PropertyId::MaterialMetallic ? 0.0f : 0.5f;
        return id == PropertyId::MaterialMetallic ? mesh->material.metallic : mesh->material.roughness;
    }

    void propertyApply(GameObject& go, const bool* escritas, const float* valores)
    {
        bool hayTransform = false;
        for (int i = 0; i <= (int)PropertyId::ScaleZ; i++) hayTransform = hayTransform || escritas[i];
        if (hayTransform)
        {
            Trs t = descompone(go.localTransform);
            for (int i = 0; i <= (int)PropertyId::ScaleZ; i++)
            {
                if (!escritas[i]) continue;
                glm::vec3& destino = i <= (int)PropertyId::PositionZ ? t.pos
                                   : (i <= (int)PropertyId::RotationZ ? t.euler : t.escala);
                destino[i % 3] = valores[i];
            }
            go.localTransform = recompone(t);
        }
        if (const auto& luz = go.getLight())
        {
            glm::vec3 color = luz->getColor();
            bool      tocaColor = false;
            for (int i = (int)PropertyId::LightColorR; i <= (int)PropertyId::LightColorB; i++)
                if (escritas[i]) { color[i - (int)PropertyId::LightColorR] = valores[i]; tocaColor = true; }
            if (tocaColor) luz->setColor(color);
            if (escritas[(int)PropertyId::LightIntensity]) luz->setIntensity(valores[(int)PropertyId::LightIntensity]);
            if (escritas[(int)PropertyId::LightRange])     luz->setRange(valores[(int)PropertyId::LightRange]);
        }
        if (escritas[(int)PropertyId::MaterialMetallic] || escritas[(int)PropertyId::MaterialRoughness])
        {
            MaterialOverride& ov = overrideParaEscribir(go);
            if (escritas[(int)PropertyId::MaterialMetallic])
                ov.metallic = glm::clamp(valores[(int)PropertyId::MaterialMetallic], 0.0f, 1.0f);
            if (escritas[(int)PropertyId::MaterialRoughness])
                ov.roughness = glm::clamp(valores[(int)PropertyId::MaterialRoughness], 0.0f, 1.0f);
        }
    }
}
