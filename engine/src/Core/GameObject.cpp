#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/MaterialAsset.h"
// Los 28 componentes se incluyen AQUÍ y no en el header (ver la nota de
// GameObject.h): el destructor de GameObject destruye los 28 shared_ptr, así
// que es esta unidad de traducción la que necesita los tipos completos.
#include "DonTopo/Renderer/Mesh.h"
#include "DonTopo/Renderer/SkinnedMesh.h"
#include "DonTopo/Physics/Colliders/BoxCollider.h"
#include "DonTopo/Physics/Colliders/SphereCollider.h"
#include "DonTopo/Physics/Colliders/CapsuleCollider.h"
#include "DonTopo/Physics/Colliders/PlaneCollider.h"
#include "DonTopo/Physics/Rigidbody.h"
#include "DonTopo/Audio/AudioClipComponent.h"
#include "DonTopo/Audio/AudioListenerComponent.h"
#include "DonTopo/Audio/ReverbZoneComponent.h"
#include "DonTopo/Core/CameraComponent.h"
#include "DonTopo/Core/AnimatorComponent.h"
#include "DonTopo/Core/ReflectionProbeComponent.h"
#include "DonTopo/Core/LightComponent.h"
#include "DonTopo/UI/CanvasComponent.h"
#include "DonTopo/UI/ButtonComponent.h"
#include "DonTopo/UI/ImageComponent.h"
#include "DonTopo/UI/LayoutComponent.h"
#include "DonTopo/UI/PanelComponent.h"
#include "DonTopo/UI/TextComponent.h"
#include "DonTopo/UI/ProgressBarComponent.h"
#include "DonTopo/UI/SliderComponent.h"
#include "DonTopo/UI/CheckboxComponent.h"
#include "DonTopo/UI/ToggleComponent.h"
#include "DonTopo/UI/ScrollbarComponent.h"
#include "DonTopo/UI/InputFieldComponent.h"
#include "DonTopo/UI/DropdownComponent.h"
#include "DonTopo/UI/ScrollViewComponent.h"
#include "DonTopo/Scripting/ScriptComponent.h"
#include <algorithm>
#include <atomic>

namespace DonTopo
{
    namespace { std::atomic<uint64_t> s_nextId{1}; }

    GameObject::GameObject(std::string name) : id(s_nextId++), name(std::move(name)) {}

    void GameObject::reserveIdAtLeast(uint64_t id)
    {
        // CAS en bucle y no un simple store: std::atomic no tiene fetch_max, y
        // leer-comparar-escribir por separado permitiría que dos hilos pisaran
        // el avance del otro. compare_exchange_weak reescribe `actual` cuando
        // falla, así que la condición del while se reevalúa con el valor bueno.
        // relaxed basta: aquí no se ordena ningún otro dato, solo se empuja un
        // contador hacia arriba.
        uint64_t actual = s_nextId.load(std::memory_order_relaxed);
        while (actual <= id &&
               !s_nextId.compare_exchange_weak(actual, id + 1,
                                               std::memory_order_relaxed,
                                               std::memory_order_relaxed))
        {
        }
    }

    uint64_t GameObject::allocateId()
    {
        // El mismo contador y el mismo orden de memoria que el constructor:
        // no es una reserva (que solo empuja un suelo), es una entrega real,
        // así que el valor devuelto no puede volver a salir de aquí.
        return s_nextId++;
    }

    GameObject::~GameObject() = default;
    GameObject::GameObject(GameObject&&) noexcept = default;
    GameObject& GameObject::operator=(GameObject&&) noexcept = default;

    bool GameObject::isSkinned() const
    {
        return m_mesh && dynamic_cast<const SkinnedMesh*>(m_mesh.get()) != nullptr;
    }

    const SkinnedMesh* GameObject::getSkinnedMesh() const
    {
        return m_mesh ? dynamic_cast<const SkinnedMesh*>(m_mesh.get()) : nullptr;
    }

    std::vector<const Material*> materialsOfMesh(const GameObject& go)
    {
        std::vector<const Material*> out;
        if (!go.hasMesh()) return out;

        // Mismo criterio que materialsOf() del Content Browser: en un skinned
        // con submallas, el Material heredado no lo mira nadie.
        if (const SkinnedMesh* sm = go.getSkinnedMesh(); sm && !sm->materials.empty())
        {
            out.reserve(sm->materials.size());
            for (const Material& m : sm->materials) out.push_back(&m);
            return out;
        }

        out.push_back(&go.getMesh()->material);
        return out;
    }

    std::vector<Material*> editMaterialsOfMesh(GameObject& go)
    {
        std::vector<Material*> out;
        if (!go.hasMesh()) return out;
        // Mismo criterio que materialsOfMesh, pero por editMesh: copia la malla
        // si está compartida antes de dar punteros escribibles.
        if (SkinnedMesh* sm = go.editSkinnedMesh(); sm && !sm->materials.empty())
        {
            out.reserve(sm->materials.size());
            for (Material& m : sm->materials) out.push_back(&m);
            return out;
        }
        out.push_back(&go.editMesh()->material);
        return out;
    }

    Mesh* GameObject::editMesh()
    {
        if (!m_mesh) return nullptr;
        if (m_mesh.use_count() > 1)
        {
            // Conserva el tipo: un skinned copiado como Mesh a secas perdería
            // esqueleto y clips sin avisar.
            if (auto* sk = dynamic_cast<const SkinnedMesh*>(m_mesh.get()))
                m_mesh = std::make_shared<SkinnedMesh>(*sk);
            else
                m_mesh = std::make_shared<Mesh>(*m_mesh);
        }
        return std::const_pointer_cast<Mesh>(m_mesh).get();
    }

    SkinnedMesh* GameObject::editSkinnedMesh()
    {
        if (!dynamic_cast<const SkinnedMesh*>(m_mesh.get())) return nullptr;
        return static_cast<SkinnedMesh*>(editMesh());
    }

    void collectMaterialOverrideWarnings(GameObject& go, std::vector<std::string>& out)
    {
        if (!go.hasMesh()) return;

        const size_t nMats = materialsOfMesh(go).size();
        for (const MaterialOverride& ov : go.materialOverrides)
        {
            // MISMA condición que la del `continue` de applyMaterialOverrides,
            // y por eso está pegada a ella en el fichero: si una de las dos se
            // toca sin la otra, el aviso deja de describir lo que de verdad se
            // ignora, que es peor que no avisar.
            if (ov.index >= 0 && static_cast<size_t>(ov.index) < nMats) continue;
            out.push_back("mesh de '" + go.name + "'.materials: index " +
                          std::to_string(ov.index) + " fuera de rango (" +
                          std::to_string(nMats) + " material(es) en el mesh), "
                          "el override de ese slot se ignora");
        }
        // Un .mat invalido no rompe la carga (se hereda todo en silencio en
        // applyMaterialOverrides, que no tiene canal de log y corre en cada
        // clon), pero SI se avisa aqui: esta funcion solo la llama el lector de
        // escena, que tiene canal para darlo.
        for (const MaterialOverride& ov : go.materialOverrides)
        {
            if (ov.matAsset.empty()) continue;
            // Aviso unico por ruta (spec): esta funcion se llama una vez POR
            // OBJETO en el mismo `out` compartido de toda la escena (ver
            // Scene::fromJson), asi que varios objetos que comparten un .mat
            // roto repetirian la misma linea sin esto.
            const std::string marker = "material '" + ov.matAsset + "': ";
            bool yaAvisado = false;
            for (const std::string& w : out)
                if (w.find(marker) != std::string::npos) { yaAvisado = true; break; }
            if (yaAvisado) continue;
            std::string warning;
            loadMaterialAsset(ov.matAsset, &warning);
            if (!warning.empty())
                out.push_back("mesh de '" + go.name + "'.materials: " + marker + warning);
        }
    }

    void applyMaterialOverrides(GameObject& go)
    {
        // Se aplica sobre una COPIA de los materiales y solo se escribe en la
        // malla si algo cambia: la malla puede estar compartida (clon, undo de
        // Delete) y escribir obliga a copiarla entera (editMesh). Un clon con
        // los mismos overrides que su original no cambia nada y la sigue
        // compartiendo. Los baselines se capturan en `ov` en esta pasada, igual
        // que antes, porque la copia tiene los mismos valores que la malla.
        const std::vector<const Material*> actuales = materialsOfMesh(go);
        std::vector<Material> copia;
        copia.reserve(actuales.size());
        for (const Material* m : actuales) copia.push_back(*m);
        std::vector<Material*> mats;
        mats.reserve(copia.size());
        for (Material& m : copia) mats.push_back(&m);

        for (MaterialOverride& ov : go.materialOverrides)
        {
            // Índice que ya no existe: el FBX se reexportó con menos submallas.
            // Se ignora en silencio aquí; el aviso lo da el lector de escena,
            // que es quien tiene canal para darlo.
            if (ov.index < 0 || ov.index >= (int)mats.size()) continue;
            Material& mat = *mats[(size_t)ov.index];

            // El .mat (si lo hay) resuelve lo que el objeto NO overridee: se
            // computa un valor "efectivo" por campo y se alimenta al MISMO
            // mecanismo de baseline de siempre, sin tocarlo. Sin matAsset,
            // matAsset queda en su defecto (todo vacio/-1) y effective(...)
            // devuelve el override del objeto tal cual: el resultado es
            // identico al de antes de que este campo existiera.
            MaterialAsset matAsset;
            if (!ov.matAsset.empty())
                matAsset = loadMaterialAsset(ov.matAsset);   // tolerante: invalido = heredar todo

            auto effective = [](const std::string& objOverride, const std::string& matValue)
            {
                return !objOverride.empty() ? objOverride : matValue;
            };
            auto effectiveFactor = [](float objOverride, float matValue)
            {
                return objOverride >= 0.0f ? objOverride : matValue;
            };

            // El baseline se captura UNA vez por slot, la primera que se pisa:
            // si se recapturase en cada pasada, el segundo cambio de textura
            // guardaría como "original" el override anterior y el Clear
            // devolvería una textura del usuario en vez de la del modelo.
            auto aplica = [](const std::string& override_, std::string& base,
                             bool& baseTomado, std::string& destino)
            {
                if (override_.empty())
                {
                    // Sin override: si alguna vez lo hubo, se vuelve al
                    // baseline. Si nunca lo hubo, no se toca nada — escribir el
                    // base vacío aquí borraría la ruta que trae el FBX.
                    if (baseTomado) destino = base;
                    return;
                }
                if (!baseTomado)
                {
                    base       = destino;
                    baseTomado = true;
                }
                destino = override_;
            };

            // Los tres flags explícitos son necesarios porque baseTomado NO se
            // puede deducir de que el slot tenga override o baseline no
            // vacíos: un baseline legítimamente vacío (mesh procedural sin
            // textura) sería indistinguible de "aún no tomado", y con la
            // heurística "base vacío = no tomado" el Clear dejaría puesta la
            // textura del usuario en vez de devolver el slot a su vacío
            // original.
            aplica(effective(ov.albedo, matAsset.albedo), ov.baseAlbedo, ov.baseAlbedoTaken, mat.texturePath);
            aplica(effective(ov.normal, matAsset.normal), ov.baseNormal, ov.baseNormalTaken, mat.normalMapPath);
            aplica(effective(ov.orm,    matAsset.orm),    ov.baseOrm,    ov.baseOrmTaken,    mat.metallicRoughnessPath);

            // Mismo mecanismo que `aplica`, pero con un centinela float en vez
            // de una cadena vacía: 0.0 y 1.0 son valores válidos de slider, así
            // que no sirven de "sin override" como sí sirve "" para una ruta.
            // -1.0 está fuera del rango 0..1 del slider (ver la nota de
            // MaterialOverride) y hace ese papel.
            auto aplicaFactor = [](float override_, float& base, bool& baseTomado, float& destino)
            {
                if (override_ < 0.0f)
                {
                    if (baseTomado) destino = base;
                    return;
                }
                if (!baseTomado)
                {
                    base       = destino;
                    baseTomado = true;
                }
                destino = override_;
            };
            aplicaFactor(effectiveFactor(ov.metallic, matAsset.metallic), ov.baseMetallic, ov.baseMetallicTaken, mat.metallic);
            aplicaFactor(effectiveFactor(ov.roughness, matAsset.roughness), ov.baseRoughness, ov.baseRoughnessTaken, mat.roughness);
        }

        auto distinto = [](const Material& a, const Material& b)
        {
            return a.texturePath != b.texturePath || a.normalMapPath != b.normalMapPath ||
                   a.metallicRoughnessPath != b.metallicRoughnessPath ||
                   a.metallic != b.metallic || a.roughness != b.roughness;
        };
        bool cambia = false;
        for (size_t i = 0; i < copia.size() && !cambia; i++)
            cambia = distinto(copia[i], *actuales[i]);
        if (!cambia) return;

        std::vector<Material*> destino = editMaterialsOfMesh(go);
        for (size_t i = 0; i < copia.size() && i < destino.size(); i++)
        {
            destino[i]->texturePath           = copia[i].texturePath;
            destino[i]->normalMapPath         = copia[i].normalMapPath;
            destino[i]->metallicRoughnessPath = copia[i].metallicRoughnessPath;
            destino[i]->metallic              = copia[i].metallic;
            destino[i]->roughness             = copia[i].roughness;
        }
    }

    std::shared_ptr<Collider> GameObject::anyCollider() const
    {
        if (m_boxCollider)     return m_boxCollider;
        if (m_sphereCollider)  return m_sphereCollider;
        if (m_capsuleCollider) return m_capsuleCollider;
        if (m_planeCollider)   return m_planeCollider;
        return nullptr;
    }

    GameObject* GameObject::addChild(std::string childName)
    {
        auto node = std::make_unique<GameObject>(std::move(childName));
        node->parent = this;
        GameObject* raw = node.get();
        children.push_back(std::move(node));
        return raw;
    }

    void GameObject::updateWorldTransforms(const glm::mat4& parentWorld)
    {
        worldTransform = parentWorld * localTransform;
        for (auto& c : children) c->updateWorldTransforms(worldTransform);
    }

    void GameObject::addScript(std::unique_ptr<ScriptComponent> script)
    {
        m_scripts.push_back(std::move(script));
    }

    void GameObject::removeScript(ScriptComponent* script)
    {
        m_scripts.erase(
            std::remove_if(m_scripts.begin(), m_scripts.end(),
                [script](const std::unique_ptr<ScriptComponent>& s) { return s.get() == script; }),
            m_scripts.end());
    }
}
