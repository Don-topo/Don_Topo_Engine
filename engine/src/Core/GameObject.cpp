#include "DonTopo/Core/GameObject.h"
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
