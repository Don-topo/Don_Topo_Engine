#include "DonTopo/Scene.h"
#include <algorithm>
#include <memory>

namespace DonTopo
{
    Scene::Scene(std::string name) : m_name(std::move(name)), m_root("root") {}

    GameObject* Scene::addGameObject(const std::string& name, GameObject* parent)
    {
        GameObject* target = parent ? parent : &m_root;
        return target->addChild(name);
    }

    void Scene::removeGameObject(GameObject* node)
    {
        if (!node || !node->parent) return;

        auto& siblings = node->parent->children;
        siblings.erase(
            std::remove_if(siblings.begin(), siblings.end(),
                [node](const std::unique_ptr<GameObject>& c) { return c.get() == node; }),
            siblings.end());
    }

    void Scene::update(float, PhysicsManager&) {}
    void Scene::shutdown(PhysicsManager&, AudioManager&) {}
}
