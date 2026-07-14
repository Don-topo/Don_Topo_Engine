#include "DonTopo/GameObject.h"
#include "DonTopo/ScriptComponent.h"
#include <algorithm>
#include <atomic>

namespace DonTopo
{
    namespace { std::atomic<uint64_t> s_nextId{1}; }

    GameObject::GameObject(std::string name) : id(s_nextId++), name(std::move(name)) {}
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
