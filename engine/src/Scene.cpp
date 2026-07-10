#include "DonTopo/Scene.h"

namespace DonTopo
{
    Scene::Scene(std::string name) : m_name(std::move(name)), m_root("root") {}

    GameObject* Scene::addGameObject(const std::string& name, GameObject* parent)
    {
        GameObject* target = parent ? parent : &m_root;
        return target->addChild(name);
    }

    void Scene::removeGameObject(GameObject*) {}
    void Scene::update(float, PhysicsManager&) {}
    void Scene::shutdown(PhysicsManager&, AudioManager&) {}
}
