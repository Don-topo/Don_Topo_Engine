#include "DonTopo/Core/GameObject.h"
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
    GameObject::~GameObject() = default;
    GameObject::GameObject(GameObject&&) noexcept = default;
    GameObject& GameObject::operator=(GameObject&&) noexcept = default;

    bool GameObject::isSkinned() const
    {
        return m_mesh && dynamic_cast<SkinnedMesh*>(m_mesh.get()) != nullptr;
    }

    SkinnedMesh* GameObject::getSkinnedMesh() const
    {
        return m_mesh ? dynamic_cast<SkinnedMesh*>(m_mesh.get()) : nullptr;
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
