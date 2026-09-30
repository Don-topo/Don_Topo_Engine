#include "DonTopo/Core/GameObject.h"
#include "DonTopo/Core/MaterialAsset.h"
// The 28 components are included HERE and not in the header (see the note in
// GameObject.h): the GameObject destructor destroys the 28 shared_ptr, so
// it is this translation unit that needs the complete types.
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
        // CAS in a loop and not a simple store: std::atomic has no fetch_max, and
        // reading-comparing-writing separately would let two threads overwrite
        // each other's advance. compare_exchange_weak rewrites `actual` when
        // it fails, so the while condition is re-evaluated with the good value.
        // relaxed is enough: no other data is ordered here, a counter is just
        // pushed upwards.
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
        // The same counter and the same memory order as the constructor:
        // it is not a reservation (which only pushes a floor), it is a real handout,
        // so the returned value cannot come out of here again.
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

        // Same criterion as materialsOf() of the Content Browser: in a skinned
        // with submeshes, nobody looks at the inherited Material.
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
        // Same criterion as materialsOfMesh, but through editMesh: it copies the mesh
        // if it is shared before handing out writable pointers.
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
            // Keeps the type: a skinned copied as a plain Mesh would lose
            // skeleton and clips without warning.
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
            // SAME condition as the `continue` of applyMaterialOverrides,
            // and that is why it is glued to it in the file: if one of the two is
            // touched without the other, the warning stops describing what is really
            // ignored, which is worse than not warning.
            if (ov.index >= 0 && static_cast<size_t>(ov.index) < nMats) continue;
            out.push_back("mesh of '" + go.name + "'.materials: index " +
                          std::to_string(ov.index) + " out of range (" +
                          std::to_string(nMats) + " material(s) in the mesh), "
                          "the override for that slot is ignored");
        }
        // An invalid .mat does not break loading (everything is silently inherited in
        // applyMaterialOverrides, which has no log channel and runs on every
        // clone), but it IS warned about here: this function is only called by the scene
        // reader, which has a channel to report it.
        for (const MaterialOverride& ov : go.materialOverrides)
        {
            if (ov.matAsset.empty()) continue;
            // Single warning per path (spec): this function is called once PER
            // OBJECT on the same shared `out` of the whole scene (see
            // Scene::fromJson), so several objects sharing a broken .mat
            // would repeat the same line without this.
            const std::string marker = "material '" + ov.matAsset + "': ";
            bool yaAvisado = false;
            for (const std::string& w : out)
                if (w.find(marker) != std::string::npos) { yaAvisado = true; break; }
            if (yaAvisado) continue;
            std::string warning;
            loadMaterialAsset(ov.matAsset, &warning);
            if (!warning.empty())
                out.push_back("mesh of '" + go.name + "'.materials: " + marker + warning);
        }
    }

    void applyMaterialOverrides(GameObject& go)
    {
        // It is applied on a COPY of the materials and only written to the
        // mesh if something changes: the mesh may be shared (clone, undo of
        // Delete) and writing forces copying it entirely (editMesh). A clone with
        // the same overrides as its original changes nothing and keeps
        // sharing it. The baselines are captured in `ov` in this pass, just
        // as before, because the copy has the same values as the mesh.
        const std::vector<const Material*> actuales = materialsOfMesh(go);
        std::vector<Material> copia;
        copia.reserve(actuales.size());
        for (const Material* m : actuales) copia.push_back(*m);
        std::vector<Material*> mats;
        mats.reserve(copia.size());
        for (Material& m : copia) mats.push_back(&m);

        for (MaterialOverride& ov : go.materialOverrides)
        {
            // Index that no longer exists: the FBX was re-exported with fewer submeshes.
            // It is silently ignored here; the warning is given by the scene reader,
            // which is the one with a channel to report it.
            if (ov.index < 0 || ov.index >= (int)mats.size()) continue;
            Material& mat = *mats[(size_t)ov.index];

            // The .mat (if any) resolves whatever the object does NOT override: an
            // "effective" value is computed per field and fed to the SAME
            // baseline mechanism as always, without touching it. Without matAsset,
            // matAsset stays at its default (all empty/-1) and effective(...)
            // returns the object's override as is: the result is
            // identical to what it was before this field existed.
            MaterialAsset matAsset;
            if (!ov.matAsset.empty())
                matAsset = loadMaterialAsset(ov.matAsset);   // tolerant: invalid = inherit everything

            auto effective = [](const std::string& objOverride, const std::string& matValue)
            {
                return !objOverride.empty() ? objOverride : matValue;
            };
            auto effectiveFactor = [](float objOverride, float matValue)
            {
                return objOverride >= 0.0f ? objOverride : matValue;
            };

            // The baseline is captured ONCE per slot, the first time it is overridden:
            // if it were recaptured on every pass, the second texture change
            // would store the previous override as the "original" and Clear
            // would return a user texture instead of the model's.
            auto aplica = [](const std::string& override_, std::string& base,
                             bool& baseTomado, std::string& destino)
            {
                if (override_.empty())
                {
                    // No override: if there ever was one, it goes back to the
                    // baseline. If there never was, nothing is touched — writing the
                    // empty base here would erase the path the FBX brings.
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

            // The three explicit flags are necessary because baseTomado CANNOT be
            // deduced from the slot having a non-empty override or baseline:
            // a legitimately empty baseline (procedural mesh with no
            // texture) would be indistinguishable from "not yet taken", and with the
            // heuristic "empty base = not taken" Clear would leave the user's
            // texture in place instead of returning the slot to its original
            // empty state.
            aplica(effective(ov.albedo, matAsset.albedo), ov.baseAlbedo, ov.baseAlbedoTaken, mat.texturePath);
            aplica(effective(ov.normal, matAsset.normal), ov.baseNormal, ov.baseNormalTaken, mat.normalMapPath);
            aplica(effective(ov.orm,    matAsset.orm),    ov.baseOrm,    ov.baseOrmTaken,    mat.metallicRoughnessPath);

            // Same mechanism as `aplica`, but with a float sentinel instead of
            // an empty string: 0.0 and 1.0 are valid slider values, so
            // they do not work as "no override" the way "" does for a path.
            // -1.0 is outside the slider's 0..1 range (see the note in
            // MaterialOverride) and plays that role.
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
