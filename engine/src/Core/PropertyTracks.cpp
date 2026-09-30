#include "DonTopo/Core/PropertyTracks.h"

#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/Renderer/Mesh.h"

#include <cmath>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
// matrix_decompose and quaternion are GTX extensions: without the macro, glm warns
// in every unit that includes them.
#ifndef GLM_ENABLE_EXPERIMENTAL
#define GLM_ENABLE_EXPERIMENTAL
#endif
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtx/quaternion.hpp>

namespace DonTopo
{
    namespace
    {
        // In the SAME order as PropertyId: the index is the enum.
        const char* const kNombres[] = {
            "position.x", "position.y", "position.z",
            "rotation.x", "rotation.y", "rotation.z",
            "scale.x",    "scale.y",    "scale.z",
            "light.color.r", "light.color.g", "light.color.b",
            "light.intensity", "light.range",
            "material.metallic", "material.roughness",
        };
        static_assert(sizeof(kNombres) / sizeof(kNombres[0]) == (size_t)PropertyId::Count,
                      "the name table must cover all of PropertyId");
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
        // The segment is found by COMPARING times, not by index: the file (or the
        // panel) may bring the keys in any order and the result
        // cannot depend on that.
        const PropertyKey* lo = nullptr;
        const PropertyKey* hi = nullptr;
        for (const auto& k : t.keys)
        {
            if (k.time <= tiempo && (!lo || k.time > lo->time)) lo = &k;
            if (k.time >= tiempo && (!hi || k.time < hi->time)) hi = &k;
        }
        if (!lo) return hi->value;      // all ahead: the first one
        if (!hi) return lo->value;      // all behind: the last one
        const float span = hi->time - lo->time;
        if (span <= 0.0f) return hi->value;
        return glm::mix(lo->value, hi->value, (tiempo - lo->time) / span);
    }

    void curveRange(const PropertyTrack& t, const float* extra, int nExtra, float& lo, float& hi)
    {
        bool hay = false;
        auto mete = [&](float v) {
            if (!hay) { lo = hi = v; hay = true; return; }
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        };
        for (const auto& k : t.keys) mete(k.value);
        for (int i = 0; i < nExtra && extra; i++) mete(extra[i]);
        if (!hay) { lo = -1.0f; hi = 1.0f; return; }
        // Zero height (flat track, or a single key): the line would end up stuck to
        // an edge and it would not be visible that it is flat.
        if (hi - lo < 1e-6f) { lo -= 0.5f; hi += 0.5f; return; }
        const float margen = (hi - lo) * 0.1f;
        lo -= margen;
        hi += margen;
    }

    CurvePoint canvasToCurve(float x, float y, float x0, float x1, float y0, float y1,
                             float duracion, float lo, float hi)
    {
        CurvePoint p;
        const float ancho = x1 - x0;
        const float alto  = y1 - y0;
        p.time  = ancho > 0.0f ? (x - x0) / ancho * duracion : 0.0f;
        p.time  = glm::clamp(p.time, 0.0f, duracion);
        // The screen grows downward and the value upward: without this
        // inversion, dragging upward would lower the value.
        p.value = alto > 0.0f ? hi - (y - y0) / alto * (hi - lo) : lo;
        return p;
    }

    void curveToCanvas(float time, float value, float x0, float x1, float y0, float y1,
                       float duracion, float lo, float hi, float& x, float& y)
    {
        const float span = hi - lo;
        x = duracion > 0.0f ? x0 + (time / duracion) * (x1 - x0) : x0;
        x = glm::clamp(x, x0, x1);      // a key beyond the clip stays at the edge
        y = span > 0.0f ? y1 - (value - lo) / span * (y1 - y0) : y1;
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
        // Angles: the DIFFERENCE with the first contribution is blended,
        // normalized to [-180, 180]. Blending them raw, 350 and 10 would give
        // 180 (arithmetic mean) instead of 0.
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

        // Translation, euler in DEGREES and scale of the localTransform.
        struct Trs { glm::vec3 pos{0.0f}, euler{0.0f}, escala{1.0f}; };
        Trs descompone(const glm::mat4& m)
        {
            Trs out;
            glm::quat rot;
            glm::vec3 skew;
            glm::vec4 persp;
            // glm::decompose with a singular matrix does NOT write its outputs, so
            // the locals are initialized above: reading them as is would give
            // garbage (-1e8) and from there a NaN that propagates to the scene.
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

        // Material factors of the OBJECT, not of the mesh: editMesh() copies the
        // mesh if it is shared, and animating cannot pay that every frame.
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
        // Every GameObject has the transform, and the material factors
        // live in the object itself: neither a mesh nor a component is needed.
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
        // Material: the override if it is active (-1 is the "no
        // override" sentinel), and otherwise the mesh value.
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
